#include <windows.h>
typedef double DATE;
#include <zorro.h>
#include "ib-cpapi.h"

int(__cdecl* BrokerMessage)(const char* Text);
int(__cdecl* BrokerProgress)(const int Progress);

extern Global G;

void registerCallbacks(FARPROC fpMessage, FARPROC fpProgress) {
	(FARPROC&)BrokerMessage = fpMessage;
	(FARPROC&)BrokerProgress = fpProgress;
}


void showMsg(const char* text, const char* detail)
{
	if (!BrokerMessage) return;
	if (!detail) detail = "";
	static char msg[1024];
	sprintf_s(msg, "%s %s", text, detail);
	BrokerMessage(msg);
}

int sleep(int ms, int par)
{
	Sleep(ms);
	return BrokerProgress(par);
}

DATE convertTime2DATE(__time32_t t32)
{
	return (double)t32 / (24. * 60. * 60.) + 25569.; // 25569. = DATE(1.1.1970 00:00)
}

DATE convertEpoch2DATE(long long t32)
{
	return (double)t32 / (24. * 60. * 60. * 1000.) + 25569.; // 25569. = DATE(1.1.1970 00:00)
}

__time32_t convertDATE2Time(DATE date)
{
	return (__time32_t)((date - 25569.) * 24. * 60. * 60.);
}

/**
 simple send method with error handling.
 chan is for selecting the max return buffer size., must be (1,0) or 2
*/
json_object* send(const char* suburl, const char* bod, const char* meth)
{
	// a place for the final url and the respone from the server
	static char url[1024], resp[1024 * 2024], header[2048];
	// wait 30 seconds for the server to reply
	int size = 0, wait = 3000;
	sprintf_s(url, "%s%s", BASEURL, suburl);
	strcpy_s(header, "Content-Type: application/json\n");
	strcpy_s(header, "User-Agent: Console\n"); // without this we get 403
	// if we add accept-Type then we cannot POST json-body 
	// method is only used when body is given. otherwise its always GET
	int reqId = http_request(url, bod, header, (bod ? (meth ? meth : "POST") : NULL));
	if (!reqId) {
		G.loggedIn = 0; // no connection to our gateway
		goto send_error;
	}
	while (!(size = http_status(reqId)) && --wait > 0) {
		if (!sleep(10)) goto send_error;
	}
	if (!size) goto send_error;
	if (!http_result(reqId, resp, sizeof(resp))) goto send_error;
	resp[sizeof(resp) - 1] = 0; // prevent buffer overrun
	http_free(reqId);
	if (strlen(resp) < 5 || !strncmp(resp, "ERROR", 5)) {
		showMsg("error with request ", strcatf(resp, suburl));
		return NULL;
	}
	return json_tokener_parse(resp);

send_error:
	if (reqId) http_free(reqId);
	return NULL;
}
