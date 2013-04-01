/******
*  init date: 2013.1.23
*  author: senbai.tao<senbai.tao@amlogic.com>
*  description: app interface for download
******/

#include "curl_log.h"
#include "curl_common.h"
#include "curl_fetch.h"

#define IPAD_IDENT  "AppleCoreMedia/1.0.0.9A405 (iPad; U; CPU OS 5_0_1 like Mac OS X; zh_cn)"

// support one uri once now, maybe add multi uri interface later

static int curl_fetch_start_local_run(CFContext * h);

static void * curl_fetch_thread_run(void *_handle);

static int curl_fetch_waitthreadquit(CFContext * h, int microseconds);

CFContext * curl_fetch_init(const char * uri, const char * headers, int flags)
{
    LOGI("curl_fetch_init enter\n");
    if (!uri || strlen(uri) < 1 || strlen(uri) > MAX_CURL_URI_SIZE) {
        LOGE("CFContext invalid uri path\n");
        return NULL;
    }
    CFContext * handle = (CFContext *)c_malloc(sizeof(CFContext));
    if (!handle) {
        LOGE("CFContext invalid\n");
        return NULL;
    }
    handle->cwc_h = curl_wrapper_init(flags);
    if (!handle->cwc_h) {
        LOGE("curl_wrapper_init failed\n");
        return NULL;
    }
#if 1
    LOGI("curl_fetch_init, uri:[%s]\n", uri);
    if (c_stristart(uri, "http://", NULL) || c_stristart(uri, "shttp://", NULL)) {
        handle->prot_type = C_PROT_HTTP;
    }
    memset(handle->uri, 0, sizeof(handle->uri));
    if (c_stristart(uri, "shttp://", NULL)) {
        c_strlcpy(handle->uri, uri + 1, sizeof(handle->uri));
    } else {
        c_strlcpy(handle->uri, uri, sizeof(handle->uri));
    }
#endif
    handle->cwd = (Curl_Data *)c_malloc(sizeof(Curl_Data));
    if (!handle->cwd) {
        LOGE("Failed to allocate memory for curl_data\n");
        return NULL;
    }
    handle->cwh_h = curl_wrapper_open(handle->cwc_h, handle->uri, headers, handle->cwd, handle->prot_type);
    if (!handle->cwh_h) {
        LOGE("curl_wrapper_open failed\n");
        return NULL;
    }
    handle->chunk = NULL;
    handle->thread_quited = 0;
    handle->perform_retval = 0;
    handle->http_code = 0;
    handle->filesize = -1;
    handle->seekable = 0;
    //handle->is_seeking = 0;
    handle->relocation = NULL;
    handle->headers = NULL;
    pthread_mutex_init(&handle->quit_mutex, NULL);
    pthread_cond_init(&handle->quit_cond, NULL);
    if (headers) {
        handle->headers = (char *)c_mallocz(strlen(headers) + 1);
        strcpy(handle->headers, headers);
    }
    return handle;
}

int curl_fetch_open(CFContext * h)
{
    LOGI("curl_fetch_open enter\n");
    if (!h) {
        LOGE("CFContext invalid\n");
        return -1;
    }
#if 1
    if (h->prot_type == C_PROT_HTTP) {
        curl_wrapper_set_para(h->cwh_h, NULL, C_MAX_REDIRECTS, 10, NULL);
        curl_wrapper_set_para(h->cwh_h, NULL, C_USER_AGENT, 0, IPAD_IDENT);
        curl_wrapper_set_para(h->cwh_h, (void *)h->cwd, C_HEADERS, 0, NULL);
    }
#endif
    if (h->headers) {
        curl_fetch_http_set_headers(h, h->headers);
    }

    /*
    int64_t tmp_size;
    if(!curl_wrapper_get_info_easy(h->cwh_h, C_INFO_CONTENT_LENGTH_DOWNLOAD, 0, &tmp_size, NULL)) {
        h->cwh_h->chunk_size = tmp_size;
    } else {
        h->cwh_h->chunk_size = -1;
    }
    h->filesize = h->cwh_h->chunk_size;
    */

    //h->thread_first_run = 1;
    curl_fetch_start_local_run(h);

    struct timeval now;
    struct timespec timeout;
    int retcode = 0;
    gettimeofday(&now, NULL);
    timeout.tv_sec = now.tv_sec + (15000000 + now.tv_usec) / 1000000;
    timeout.tv_nsec = now.tv_usec * 1000;
    pthread_mutex_lock(&h->cwh_h->info_mutex);
    retcode = pthread_cond_timedwait(&h->cwh_h->info_cond, &h->cwh_h->info_mutex, &timeout);
    if (retcode != ETIMEDOUT) {
        if (h->cwh_h->chunk_size > 0) {
            h->filesize = h->cwh_h->chunk_size;
        }
        if (h->cwh_h->relocation) {
            h->relocation = h->cwh_h->relocation;
        }
        h->http_code = h->cwh_h->http_code;
    }
    pthread_mutex_unlock(&h->cwh_h->info_mutex);

    if (h->http_code >= 400 && h->http_code < 600 && h->http_code != 401) {
        return -1;
    }

    if (h->filesize > 0 || h->cwh_h->seekable) {
        h->seekable = 1;
    }

    return 0;
}

int curl_fetch_http_keepalive_open(CFContext * h, const char * uri)
{
    LOGI("curl_fetch_http_keepalive_open enter\n");
    int ret = -1;
    if (!h) {
        LOGE("CFContext invalid\n");
        return ret;
    }
    curl_wrapper_set_to_quit(h->cwc_h, NULL);
    if (curl_fetch_waitthreadquit(h, 5000 * 1000)) {
        return ret;
    }
    curl_wrapper_clean_after_perform(h->cwc_h);
    if (h->cwh_h->cfifo) {
        curl_fifo_reset(h->cwh_h->cfifo);
    }
    if (uri) {
        LOGI("curl_fetch_http_keepalive_open, uri:[%s]\n", uri);
        if (c_stristart(uri, "http://", NULL) || c_stristart(uri, "shttp://", NULL)) {
            h->prot_type = C_PROT_HTTP;
        }
        if (h->prot_type != C_PROT_HTTP) {
            return ret;
        }
        memset(h->uri, 0, sizeof(h->uri));
        if (c_stristart(uri, "shttp://", NULL)) {
            c_strlcpy(h->uri, uri + 1, sizeof(h->uri));
        } else {
            c_strlcpy(h->uri, uri, sizeof(h->uri));
        }
        ret = curl_wrapper_http_keepalive_open(h->cwc_h, h->cwh_h, h->uri);
    } else {
        ret = curl_wrapper_http_keepalive_open(h->cwc_h, h->cwh_h, uri);
    }
    if (-1 == ret) {
        LOGE("curl_wrapper_http_keepalive_open failed\n");
        return ret;
    }
    h->thread_quited = 0;
    h->perform_retval = 0;
    h->http_code = 0;
    h->filesize = -1;
    h->seekable = 0;
    //h->is_seeking = 0;
    h->cwd->size = 0;

    curl_fetch_start_local_run(h);

    struct timeval now;
    struct timespec timeout;
    int retcode = 0;
    gettimeofday(&now, NULL);
    timeout.tv_sec = now.tv_sec + (15000000 + now.tv_usec) / 1000000;
    timeout.tv_nsec = now.tv_usec * 1000;
    pthread_mutex_lock(&h->cwh_h->info_mutex);
    retcode = pthread_cond_timedwait(&h->cwh_h->info_cond, &h->cwh_h->info_mutex, &timeout);
    if (retcode != ETIMEDOUT) {
        if (h->cwh_h->chunk_size > 0) {
            h->filesize = h->cwh_h->chunk_size;
        }
        if (h->cwh_h->relocation) {
            h->relocation = h->cwh_h->relocation;
        }
        h->http_code = h->cwh_h->http_code;
    }
    pthread_mutex_unlock(&h->cwh_h->info_mutex);

    if (h->http_code >= 400 && h->http_code < 600 && h->http_code != 401) {
        return ret;
    }

    if (h->filesize > 0 || h->cwh_h->seekable) {
        h->seekable = 1;
    }

    ret = 0;
    return ret;
}

static int curl_fetch_start_local_run(CFContext * h)
{
    LOGI("curl_fetch_start_local_run enter\n");
    int ret = -1;
    if (!h) {
        LOGE("CFContext invalid\n");
        return ret;
    }
    ret = pthread_create(&h->pid, NULL, curl_fetch_thread_run, h);
    return ret;
}

static void * curl_fetch_thread_run(void *_handle)
{
    LOGI("curl_fetch_thread_run enter\n");
    CFContext * h = (CFContext *)_handle;
#if 0
    if (h->thread_first_run) {  // care of instant seeking after open
        h->thread_first_run = 0;
        usleep(20 * 1000);
        if (h->is_seeking) {
            h->thread_quited = 1;
            return NULL;
        }
    }
#endif
    h->perform_retval = curl_wrapper_perform(h->cwc_h);
    pthread_mutex_lock(&h->quit_mutex);
    h->thread_quited = 1;
    pthread_cond_signal(&h->quit_cond);
    pthread_mutex_unlock(&h->quit_mutex);
    LOGI("curl_fetch_thread_run quit\n");
    return NULL;
}

static int curl_fetch_waitthreadquit(CFContext * h, int microseconds)
{
    LOGI("curl_fetch_waitthreadquit enter\n");
    if (!h) {
        return -1;
    }
    struct timeval now;
    struct timespec timeout;
    int retcode = 0;
    gettimeofday(&now, NULL);
    timeout.tv_sec = now.tv_sec + (microseconds + now.tv_usec) / 1000000;
    timeout.tv_nsec = now.tv_usec * 1000;
    pthread_mutex_lock(&h->quit_mutex);
    while (!h->thread_quited && retcode != ETIMEDOUT) {
        retcode = pthread_cond_timedwait(&h->quit_cond, &h->quit_mutex, &timeout);
        if (retcode == ETIMEDOUT) {
            LOGI("curl_fetch_waitthreadquit wait too long\n");
            pthread_mutex_unlock(&h->quit_mutex);
            return -1;
        }
    }
    pthread_mutex_unlock(&h->quit_mutex);
    return 0;
}

int curl_fetch_read(CFContext * h, char * buf, int size)
{
    if (!h) {
        LOGE("CFContext invalid\n");
        return C_ERROR_UNKNOW;
    }
    if (!h->cwh_h->cfifo) {
        LOGE("CURLWHandle fifo invalid\n");
        return C_ERROR_UNKNOW;
    }
    int avail = 0;
    pthread_mutex_lock(&h->cwh_h->fifo_mutex);
    avail = curl_fifo_size(h->cwh_h->cfifo);
    if (avail) {
        size = CURLMIN(avail, size);
        curl_fifo_generic_read(h->cwh_h->cfifo, buf, size, NULL);
        pthread_cond_signal(&h->cwh_h->pthread_cond);
        pthread_mutex_unlock(&h->cwh_h->fifo_mutex);
        return size;
    } else if (h->thread_quited) {
        pthread_mutex_unlock(&h->cwh_h->fifo_mutex);
        return h->perform_retval;
    } else {
        pthread_mutex_unlock(&h->cwh_h->fifo_mutex);
        return C_ERROR_EAGAIN;
    }
    pthread_mutex_unlock(&h->cwh_h->fifo_mutex);
    return C_ERROR_OK;
}

int64_t curl_fetch_seek(CFContext * h, int64_t off, int whence)
{
    LOGI("curl_fetch_seek enter\n");
    if (!h) {
        LOGE("CFContext invalid\n");
        return -1;
    }
    LOGI("curl_fetch_seek: chunk_size=%lld, off=%lld, whence=%d\n", h->cwh_h->chunk_size, off, whence);
    if (SEEK_CUR != whence &&
        SEEK_SET != whence &&
        SEEK_END != whence) {
        LOGE("curl_fetch_seek whence not support\n");
        return -1;
    }
    if (h->cwh_h->chunk_size == -1 && whence == SEEK_END) {
        return -1;
    }
    if (whence == SEEK_CUR && off == 0) {
        return h->cwd->size;        //just a approximate value, not exactly
    }
    if (whence == SEEK_CUR) {
        off += h->cwd->size;
    } else if (whence == SEEK_END) {
        off += h->cwh_h->chunk_size;
    }
    if (off >= h->cwh_h->chunk_size && h->cwh_h->chunk_size > 0) {
        LOGE("curl_fetch_seek exceed chunk_size\n");
        return -2;
    }
    int ret = -1;
    //h->is_seeking = 1;
    curl_wrapper_set_to_quit(h->cwc_h, NULL);
    if (curl_fetch_waitthreadquit(h, 5000 * 1000)) {
        return -1;
    }
    curl_wrapper_clean_after_perform(h->cwc_h);
    h->thread_quited = 0;
    h->perform_retval = 0;
    if (h->cwh_h->cfifo) {
        curl_fifo_reset(h->cwh_h->cfifo);
    }
    ret = curl_wrapper_seek(h->cwc_h, h->cwh_h, off, h->cwd, h->prot_type);
    if (ret) {
        LOGE("curl_wrapper_seek failed\n");
        return -1;
    }
    if (h->headers) {
        curl_fetch_http_set_headers(h, h->headers);
    }
#if 1
    if (h->prot_type == C_PROT_HTTP) {
        curl_wrapper_set_para(h->cwh_h, NULL, C_MAX_REDIRECTS, 10, NULL);
        curl_wrapper_set_para(h->cwh_h, NULL, C_USER_AGENT, 0, IPAD_IDENT);
        curl_wrapper_set_para(h->cwh_h, (void *)h->cwd, C_HEADERS, 0, NULL);
    }
#endif
    ret = curl_fetch_start_local_run(h);
    if (ret) {
        LOGE("curl_fetch_start_local_run failed\n");
        return -1;
    }
    return off;
}

int curl_fetch_close(CFContext * h)
{
    LOGI("curl_fetch_close enter\n");
    if (!h) {
        LOGE("CFContext invalid\n");
        return -1;
    }
    curl_wrapper_set_to_quit(h->cwc_h, NULL);
    pthread_join(h->pid, NULL);
    curl_wrapper_clean_after_perform(h->cwc_h);
    pthread_mutex_destroy(&h->quit_mutex);
    pthread_cond_destroy(&h->quit_cond);
    curl_wrapper_close(h->cwc_h);
    if (h->cwd) {
        c_free(h->cwd);
        h->cwd = NULL;
    }
    if (h->headers) {
        c_free(h->headers);
        h->headers = NULL;
    }
    curl_slist_free_all(h->chunk);
    c_free(h);
    h = NULL;
    return 0;
}

/*
*   headers must composed of fields split with "\r\n".
*   This is just temporary, need to modify later
*/
int curl_fetch_http_set_headers(CFContext * h, const char * headers)
{
    LOGI("curl_fetch_http_set_headers enter\n");
    if (!h) {
        LOGE("CFContext invalid\n");
        return -1;
    }
    char fields[2048];
    char * end_ptr = headers;
    char * beg_ptr = headers;
    while (*beg_ptr != '\0') {
        if (!(end_ptr = strstr(beg_ptr, "\r\n"))) {
            break;
        }
        if (beg_ptr == end_ptr) {
            beg_ptr += 2;
            continue;
        }
        memset(fields, 0x00, sizeof(fields));
        int tmp = CURLMIN(end_ptr - beg_ptr + 1, sizeof(fields) - 1);
        c_strlcpy(fields, beg_ptr, tmp);
        fields[tmp] = '\0';
        /*
        char * tmp_ptr = c_strrstr(fields, "\r\n"); // must remove CRLF
        if(tmp_ptr) {
            *tmp_ptr = '\0';
        }
        beg_ptr = end_ptr + 5;
        */
        beg_ptr = end_ptr + 2;
#if 0
        if (c_stristart(fields, "Connection:", NULL) || c_stristart(fields, "X-Playback-Session-Id:", NULL)) {
            h->chunk = curl_slist_append(h->chunk, fields);
            LOGI("curl_fetch_http_set_headers fields=[%s]", fields);
        }
#else
        h->chunk = curl_slist_append(h->chunk, fields);
        LOGI("curl_fetch_http_set_headers fields=[%s]", fields);
#endif
    }
    int ret = 0;
    if (h->prot_type == C_PROT_HTTP) {
        ret = curl_wrapper_set_para(h->cwh_h, (void *)h->chunk, C_HTTPHEADER, 0, NULL);
    }
    return ret;
}

int curl_fetch_http_set_cookie(CFContext * h, const char * cookie)
{
    LOGI("curl_fetch_http_set_cookie enter\n");
    if (!h) {
        LOGE("CFContext invalid\n");
        return -1;
    }
    int ret = 0;
    if (h->prot_type == C_PROT_HTTP) {
        ret = curl_wrapper_set_para(h->cwh_h, NULL, C_COOKIES, 0, cookie);
    }
    return ret;
}

int curl_fetch_get_info(CFContext * h, curl_info cmd, uint32_t flag, void * info)
{
    LOGI("curl_fetch_get_info enter\n");
    return 0;
}