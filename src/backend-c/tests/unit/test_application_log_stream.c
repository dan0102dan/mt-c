/* Exercise real journal + SSE handlers with a test-local HTTP boundary.
 * CI links the real JSON/base64 implementations from LIB_OBJS. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define mt_http_req_method log_test_method
#define mt_http_req_query log_test_query
#define mt_http_req_body log_test_body
#define mt_http_res_write_error log_test_error
#define mt_http_res_set_header log_test_header
#define mt_http_res_write_json log_test_json
#define mt_http_stream_open log_test_open
#define mt_http_stream_watch_fd log_test_watch
#include "magitrickle/http_stream.h"
#include "magitrickle/json.h"
#include "magitrickle/log_history.h"
#include "magitrickle/hash.h"
struct mt_http_req { const char *method, *body, *session, *after; };
struct mt_http_res { int status; cJSON *json; };
struct mt_http_stream { int unused; };
static struct { mt_http_stream_next_fn next; mt_http_stream_close_fn close; void *ud; } opened;
static struct mt_http_stream dummy;
static int routes;
static FILE *dump;
const char *log_test_method(const mt_http_req_t *r) { return r->method; }
const char *log_test_query(const mt_http_req_t *r,const char *key) {
    return strcmp(key,"session")==0?r->session:strcmp(key,"after")==0?r->after:NULL;
}
const uint8_t *log_test_body(const mt_http_req_t *r,size_t *len) {
    *len=r->body?strlen(r->body):0;return (const uint8_t *)r->body;
}
void log_test_error(mt_http_res_t *r,int code,const char *message) {(void)message;r->status=code;}
void log_test_header(mt_http_res_t *r,const char *name,const char *value) {(void)r;(void)name;(void)value;}
bool log_test_json(mt_http_res_t *r,int code,cJSON *json) {r->status=code;r->json=json;return true;}
mt_http_stream_t *log_test_open(mt_http_req_t *r,mt_http_res_t *res,
    mt_http_stream_next_fn next,mt_http_stream_close_fn close,void *ud) {
    (void)r;res->status=200;opened.next=next;opened.close=close;opened.ud=ud;return &dummy;
}
void log_test_watch(mt_http_stream_t *s,int fd) {(void)s;assert(fcntl(fd,F_GETFL)&O_NONBLOCK);}
static void must_route(mt_httpd_t *h,const char *method,const char *path,mt_http_handler_fn fn,void *ud) {
    (void)h;(void)method;(void)ud;assert(strstr(path,"/api/v1/logs/")==path);assert(fn);routes++;
}
#include "../../src/api/application_logs.inc"

static cJSON *next_event(const char *event) {
    size_t len=0;char *text=opened.next(opened.ud,&len);
    assert(text && len>0 && len<MT_HTTP_STREAM_MAX_FRAME);
    if(dump) assert(fwrite(text,1,len,dump)==len);
    assert(strncmp(text,"event: ",7)==0);
    assert(strncmp(text+7,event,strlen(event))==0);
    char *data=strstr(text,"\ndata: ");assert(data);
    cJSON *json=cJSON_Parse(data+7);assert(json);free(text);return json;
}
static void consume(const char *event) {cJSON_Delete(next_event(event));}
static void close_stream(void) {opened.close(opened.ud);memset(&opened,0,sizeof(opened));}
static void test_controls(void) {
    application_logs_register(NULL);assert(routes==3);
    mt_http_req_t req={.method="GET"};mt_http_res_t res={0};
    handle_application_capture(&req,&res,NULL);
    assert(res.status==200 && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(res.json,"enabled")));
    cJSON_Delete(res.json);
    const char *bad[]={"{}","{\"enabled\":1}","{\"enabled\":true,\"extra\":0}",
        "{\"enabled\":true,\"enabled\":false}","{\"enabled\":true} garbage"};
    req.method="PUT";
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++){
        req.body=bad[i];res=(mt_http_res_t){0};handle_application_capture(&req,&res,NULL);assert(res.status==400);
    }
    mt_log_history_state_t state;mt_log_history_state(&state);assert(!state.enabled);
    req.body="{\"enabled\":true}";res=(mt_http_res_t){0};
    handle_application_capture(&req,&res,NULL);assert(res.status==200);cJSON_Delete(res.json);
}
static void test_replay_and_resume(void) {
    MT_INFO("first recorded message");
    char *message=malloc(140001);assert(message);memset(message,'x',140000);message[140000]=0;
    memcpy(message+139992,"THE-END!",8);MT_WARN("%s",message);
    mt_log_history_state_t state;mt_log_history_state(&state);assert(state.count==2);
    mt_http_req_t req={.method="GET"};mt_http_res_t res={0};
    handle_application_stream(&req,&res,NULL);assert(res.status==200 && opened.ud);
    const char *path=getenv("MT_LOG_TEST_DUMP");if(path){dump=fopen(path,"wb");assert(dump);}
    consume("capture");consume("log");
    size_t received=0;char *bytes=malloc(140026);assert(bytes);
    while(received<140026){
        cJSON *json=next_event("log");
        const cJSON *data=cJSON_GetObjectItemCaseSensitive(json,"data");
        assert(strcmp(cJSON_GetObjectItemCaseSensitive(json,"id")->valuestring,"2")==0);
        assert(cJSON_GetObjectItemCaseSensitive(json,"offset")->valuedouble==(double)received);
        assert(cJSON_GetObjectItemCaseSensitive(json,"length")->valuedouble==140026);
        size_t n=0;
        assert(mt_base64_decode(data->valuestring,strlen(data->valuestring),(uint8_t *)bytes+received,140026-received,&n)==0);
        received+=n;
        assert(!!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json,"end"))==(received==140026));
        cJSON_Delete(json);
    }
    assert(memcmp(bytes+25,message,140000)==0 && bytes[140025]=='\n');
    consume("ready");
    if(dump){fclose(dump);dump=NULL;}
    size_t length=99;assert(opened.next(opened.ud,&length)==NULL && length==0);
    MT_INFO("live");consume("log");close_stream();
    req.session=state.session;req.after="2";res=(mt_http_res_t){0};
    handle_application_stream(&req,&res,NULL);assert(res.status==200);consume("capture");
    cJSON *json=next_event("log");assert(strcmp(cJSON_GetObjectItemCaseSensitive(json,"id")->valuestring,"3")==0);
    cJSON_Delete(json);consume("ready");
    assert(mt_log_history_enable(false));consume("capture");consume("ready");
    assert(mt_log_history_enable(true));consume("capture");consume("ready");
    MT_INFO("new session");json=next_event("log");
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(json,"id")->valuestring,"1")==0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(json,"session")->valuestring,state.session)!=0);
    cJSON_Delete(json);close_stream();free(bytes);free(message);
}
static void test_bad_cursors(void) {
    const char *bad[]={"-1","+1","01","18446744073709551616","bad"};
    mt_log_history_state_t state;mt_log_history_state(&state);
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++){
        mt_http_req_t r={.method="GET",.session=state.session,.after=bad[i]};mt_http_res_t res={0};
        handle_application_stream(&r,&res,NULL);assert(res.status==400);
    }
    mt_http_req_t r={.method="GET",.session=state.session,.after="999"};mt_http_res_t res={0};
    handle_application_stream(&r,&res,NULL);assert(res.status==400);
}
int main(void) {
    int fd=open("/dev/null",O_WRONLY);assert(fd>=0);mt_log_set_fd(fd);
    assert(mt_log_history_enable(false));test_controls();test_replay_and_resume();test_bad_cursors();
    assert(mt_log_history_enable(false));mt_log_set_fd(STDOUT_FILENO);close(fd);
    puts("application log stream: controls, replay, large chunks, resume, stop/start and invalid cursors passed");
    return 0;
}
