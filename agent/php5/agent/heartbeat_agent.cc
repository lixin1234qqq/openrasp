/*
 * Copyright 2017-2019 Baidu Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "utils/json_reader.h"
#include "utils/file.h"
#include "openrasp_agent.h"
#include "openrasp_hook.h"
#include "utils/digest.h"
#include "utils/hostname.h"
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <algorithm>
#include "shared_config_manager.h"
#include "agent/utils/os.h"

namespace openrasp
{

volatile int HeartBeatAgent::signal_received = 0;
static const std::string heartbeat_url_path = "/v1/agent/heartbeat";
static const std::string HEARTBEAT_AGENT_PR_NAME = "rasp-heartbeat";

HeartBeatAgent::HeartBeatAgent()
	: BaseAgent(HEARTBEAT_AGENT_PR_NAME)
{
}

void HeartBeatAgent::run()
{
	pid_t supervisor_pid = getppid();
	AGENT_SET_PROC_NAME(this->name.c_str());
	install_signal_handler(
		[](int signal_no) {
			HeartBeatAgent::signal_received = signal_no;
		});

	while (true)
	{
		update_log_level();
		if (do_heartbeat())
		{
			continue;
		}

		for (long i = 0; i < openrasp_ini.heartbeat_interval; ++i)
		{
			sleep(1);
			if (!pid_alive(std::to_string(oam->get_master_pid())) ||
				!pid_alive(std::to_string(supervisor_pid)) ||
				HeartBeatAgent::signal_received == SIGTERM)
			{
				exit(0);
			}
		}
	}
}

bool HeartBeatAgent::do_heartbeat()
{
	bool result = false;
	std::string url_string = std::string(openrasp_ini.backend_url) + heartbeat_url_path;

	JsonReader json_reader;
	json_reader.write_string({"rasp_id"}, scm->get_rasp_id());
	json_reader.write_string({"hostname"}, get_hostname());
	json_reader.write_string({"plugin_md5"}, oam->get_plugin_md5());
	json_reader.write_string({"plugin_name"}, oam->get_plugin_name());
	json_reader.write_string({"plugin_version"}, oam->get_plugin_version());
	json_reader.write_int64({"config_time"}, scm->get_config_last_update());
	std::string json_content = json_reader.dump();

	BackendRequest backend_request;
	backend_request.set_url(url_string);
	backend_request.add_post_fields(json_content);
	openrasp_error(LEVEL_DEBUG, HEARTBEAT_ERROR, _("url:%s body:%s"), url_string.c_str(), json_content.c_str());
	std::shared_ptr<BackendResponse> res_info = backend_request.curl_perform();
	if (!res_info)
	{
		openrasp_error(LEVEL_WARNING, HEARTBEAT_ERROR, _("CURL error: %s (%d), url: %s"),
					   backend_request.get_curl_err_msg(), backend_request.get_curl_code(),
					   url_string.c_str());
		return result;
	}
	openrasp_error(LEVEL_DEBUG, HEARTBEAT_ERROR, _("%s"), res_info->to_string().c_str());
	if (res_info->has_error())
	{
		openrasp_error(LEVEL_WARNING, HEARTBEAT_ERROR, _("Fail to parse response body, error message %s."),
					   res_info->get_error_msg().c_str());
		return result;
	}
	if (!res_info->http_code_ok())
	{
		openrasp_error(LEVEL_WARNING, HEARTBEAT_ERROR, _("Unexpected http response code: %ld."),
					   res_info->get_http_code());
		return result;
	}
	BaseReader *body_reader = res_info->get_body_reader();
	if (nullptr == body_reader)
	{
		return result;
	}
	int64_t status = body_reader->fetch_int64({"status"}, BackendResponse::default_int64);
	std::string description = body_reader->fetch_string({"description"}, "");
	if (4001 == status)
	{
		oam->set_registered(false);
		openrasp_error(LEVEL_WARNING, HEARTBEAT_ERROR, _("Need register again, description: %s"),
					   description.c_str());
		return result;
	}
	else if (0 != status)
	{
		openrasp_error(LEVEL_WARNING, HEARTBEAT_ERROR, _("API error: %ld, description: %s"),
					   status, description.c_str());
		return result;
	}

	if (oam->get_registered())
	{
		/************************************plugin update************************************/
		std::shared_ptr<PluginUpdatePackage> plugin_update_pkg = build_plugin_update_package(body_reader);
		if (plugin_update_pkg)
		{
			if (plugin_update_pkg->build_snapshot())
			{
				oam->set_plugin_md5(plugin_update_pkg->get_md5().c_str());
				oam->set_plugin_name(plugin_update_pkg->get_name().c_str());
				oam->set_plugin_version(plugin_update_pkg->get_version().c_str());
				openrasp_error(LEVEL_DEBUG, HEARTBEAT_ERROR, _("Successfully build snapshot, version: %s, md5: %s."),
							   plugin_update_pkg->get_version().c_str(), plugin_update_pkg->get_md5().c_str());
				result = true;
			}
		}

		/************************************config update************************************/
		int64_t config_time = body_reader->fetch_int64({"data", "config_time"});
		//timestamp should 1, greater than zero; 2, different from local config_time
		if (config_time >= 0 && scm->get_config_last_update() != config_time)
		{
			std::string complete_config = body_reader->dump({"data", "config"}, true);
			if (!complete_config.empty())
			{
				/************************************shm config************************************/
				//update log_max_backup only its value greater than zero
				int64_t log_max_backup = body_reader->fetch_int64({"data", "config", "log.maxbackup"});
				scm->set_log_max_backup(log_max_backup > 0 ? log_max_backup : 30);
				int64_t debug_level = body_reader->fetch_int64({"data", "config", "debug.level"}, 0);
				scm->set_debug_level(debug_level);
				std::map<std::string, std::vector<std::string>> white_map = build_hook_white_map({"data", "config", "hook.white"}, body_reader);
				scm->build_check_type_white_array(white_map);

				/************************************OPENRASP_G(config)************************************/
				std::string cloud_config_file_path = std::string(openrasp_ini.root_dir) + "/conf/cloud-config.json";
#ifndef _WIN32
				mode_t oldmask = umask(0);
#endif
				bool write_ok = write_string_to_file(cloud_config_file_path.c_str(),
													 std::ofstream::in | std::ofstream::out | std::ofstream::trunc,
													 complete_config.c_str(),
													 complete_config.length());
#ifndef _WIN32
				umask(oldmask);
#endif
				if (write_ok)
				{
					scm->set_config_last_update(config_time);
					openrasp_error(LEVEL_DEBUG, HEARTBEAT_ERROR, _("Successfully update config, config time: %ld."),
								   config_time);
					result = true;
				}
				else
				{
					openrasp_error(LEVEL_WARNING, HEARTBEAT_ERROR, _("Fail to write cloud config to %s, error %s."),
								   cloud_config_file_path.c_str(), strerror(errno));
				}
			}
		}
	}
	return result;
}

void HeartBeatAgent::write_pid_to_shm(pid_t agent_pid)
{
	oam->set_plugin_agent_id(agent_pid);
}

pid_t HeartBeatAgent::get_pid_from_shm()
{
	return oam->get_plugin_agent_id();
}

std::shared_ptr<PluginUpdatePackage> HeartBeatAgent::build_plugin_update_package(BaseReader *body_reader)
{
	std::shared_ptr<PluginUpdatePackage> result = nullptr;
	if (nullptr != body_reader)
	{
		std::string plugin = body_reader->fetch_string({"data", "plugin", "plugin"}, "");
		std::string md5 = body_reader->fetch_string({"data", "plugin", "md5"}, "");
		if (!plugin.empty() && !md5.empty() && md5 != oam->get_plugin_md5())
		{
			std::string cal_md5 =
				md5sum(static_cast<const void *>(plugin.c_str()), plugin.length());
			if (cal_md5 != md5)
			{
				return nullptr;
			}
			std::string version = body_reader->fetch_string({"data", "plugin", "version"}, "");
			std::string name = body_reader->fetch_string({"data", "plugin", "name"}, "");
			if (version.empty() || name.empty())
			{
				return nullptr;
			}
			result = make_shared<PluginUpdatePackage>(plugin, version, name, md5);
		}
	}
	return result;
}

std::map<std::string, std::vector<std::string>> HeartBeatAgent::build_hook_white_map(const std::vector<std::string> &keys, BaseReader *body_reader)
{
	std::map<std::string, std::vector<std::string>> hook_white_map;
	if (nullptr != body_reader)
	{
		std::vector<std::string> url_keys = body_reader->fetch_object_keys(keys);
		std::vector<std::string> tmp_keys(keys);
		for (auto &key_item : url_keys)
		{
			tmp_keys.push_back(key_item);
			std::vector<std::string> white_types = body_reader->fetch_strings(tmp_keys);
			tmp_keys.pop_back();
			hook_white_map.insert({key_item, white_types});
		}
	}
	return hook_white_map;
}

} // namespace openrasp
