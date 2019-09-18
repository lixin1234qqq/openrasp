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

#include "openrasp_hook.h"

/**
 * fileupload相关hook点
 */
PRE_HOOK_FUNCTION(move_uploaded_file, FILE_UPLOAD);

void pre_global_move_uploaded_file_FILE_UPLOAD(OPENRASP_INTERNAL_FUNCTION_PARAMETERS)
{
    char *path = nullptr;
    char *new_path = nullptr;
    size_t path_len = 0;
    size_t new_path_len = 0;
    if (!SG(rfc1867_uploaded_files))
    {
        return;
    }
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "sp", &path, &path_len, &new_path, &new_path_len) == FAILURE)
    {
        return;
    }

    if (!zend_hash_str_exists(SG(rfc1867_uploaded_files), path, path_len) ||
        php_check_open_basedir(new_path) ||
        (Z_TYPE(PG(http_globals)[TRACK_VARS_FILES]) != IS_STRING && !zend_is_auto_global_str(ZEND_STRL("_FILES"))))
    {
        return;
    }

    zval *realname = nullptr;
    zval *file = nullptr;
    zend_string *key = nullptr;
    std::string form_data_name;
    std::string realname_str;
    zend_ulong idx;
    ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL(PG(http_globals)[TRACK_VARS_FILES]), idx, key, file)
    {
        if (key != nullptr)
        {
            form_data_name = std::string(ZSTR_VAL(key));
        }
        else
        {
            zend_long actual = idx;
            form_data_name = std::to_string(actual);
        }
        zval *tmp_name = nullptr;
        if (Z_TYPE_P(file) == IS_ARRAY &&
            (tmp_name = zend_hash_str_find(Z_ARRVAL_P(file), ZEND_STRL("tmp_name"))) != nullptr)
        {
            if (Z_TYPE_P(tmp_name) == IS_STRING)
            {
                if (zend_binary_strcmp(Z_STRVAL_P(tmp_name), Z_STRLEN_P(tmp_name), path, path_len) == 0)
                {
                    if ((realname = zend_hash_str_find(Z_ARRVAL_P(file), ZEND_STRL("name"))) != nullptr &&
                        IS_STRING == Z_TYPE_P(realname))
                    {
                        realname_str = std::string(Z_STRVAL_P(realname), Z_STRLEN_P(realname));
                        break;
                    }
                }
            }
            else if (Z_TYPE_P(tmp_name) == IS_ARRAY)
            {
                zval *tmp_name_file = nullptr;
                zend_string *tmp_name_key = nullptr;
                zend_ulong tmp_name_idx;
                bool found = false;
                ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(tmp_name), tmp_name_idx, tmp_name_key, tmp_name_file)
                {
                    if (Z_TYPE_P(tmp_name_file) == IS_STRING &&
                        zend_binary_strcmp(Z_STRVAL_P(tmp_name_file), Z_STRLEN_P(tmp_name_file), path, path_len) == 0)
                    {
                        found = true;
                        break;
                    }
                }
                ZEND_HASH_FOREACH_END();
                if (found)
                {
                    if ((realname = zend_hash_str_find(Z_ARRVAL_P(file), ZEND_STRL("name"))) != nullptr &&
                        IS_ARRAY == Z_TYPE_P(realname))
                    {
                        zval *realname_item = nullptr;
                        if (tmp_name_key != nullptr)
                        {
                            if ((realname_item = zend_hash_find(Z_ARRVAL_P(realname), tmp_name_key)) != nullptr &&
                                IS_STRING == Z_TYPE_P(realname_item))
                            {
                                realname_str = std::string(Z_STRVAL_P(realname_item), Z_STRLEN_P(realname_item));
                                break;
                            }
                        }
                        else
                        {
                            if ((realname_item = zend_hash_index_find(Z_ARRVAL_P(realname), tmp_name_idx)) != nullptr &&
                                IS_STRING == Z_TYPE_P(realname_item))
                            {
                                realname_str = std::string(Z_STRVAL_P(realname_item), Z_STRLEN_P(realname_item));
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    ZEND_HASH_FOREACH_END();
    php_stream *stream = php_stream_open_wrapper(path, "rb", 0, nullptr);
    if (stream)
    {
        zend_string *buffer = php_stream_copy_to_mem(stream, 4 * 1024, 0);
        stream->is_persistent ? php_stream_pclose(stream) : php_stream_close(stream);
        if (buffer)
        {
            openrasp::CheckResult check_result = openrasp::CheckResult::kCache;
            openrasp::Isolate *isolate = OPENRASP_V8_G(isolate);
            std::string real_dest = openrasp_real_path(new_path, new_path_len, 0, WRITING);
            if (isolate)
            {
                v8::HandleScope handle_scope(isolate);
                auto params = v8::Object::New(isolate);
                params->Set(openrasp::NewV8String(isolate, "name"), openrasp::NewV8String(isolate, form_data_name));
                params->Set(openrasp::NewV8String(isolate, "filename"), openrasp::NewV8String(isolate, realname_str));
                params->Set(openrasp::NewV8String(isolate, "dest_path"), openrasp::NewV8String(isolate, new_path, new_path_len));
                params->Set(openrasp::NewV8String(isolate, "dest_realpath"), openrasp::NewV8String(isolate, real_dest));
                params->Set(openrasp::NewV8String(isolate, "content"), openrasp::NewV8String(isolate, buffer->val, MIN(buffer->len, 4 * 1024)));
                check_result = Check(isolate, openrasp::NewV8String(isolate, get_check_type_name(check_type)), params, OPENRASP_CONFIG(plugin.timeout.millis));
            }
            zend_string_release(buffer);
            if (check_result == openrasp::CheckResult::kBlock)
            {
                handle_block();
            }
        }
    }
}