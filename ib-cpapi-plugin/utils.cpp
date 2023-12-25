#include <windows.h>
typedef double DATE;
#include <zorro.h>
#include "ib-cpapi.h"

int(__cdecl* BrokerMessage)(const char* Text);
int(__cdecl* BrokerProgress)(const int Progress);

extern Global G;

// the current Asset from IB, taken from Hashmap
exchg_rate* EXCHG_Rates = NULL;


void registerCallbacks(FARPROC fpMessage, FARPROC fpProgress)
{
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

/*
* fill the given struct with the info parsed from the asset
*/
void decompose_zorro_asset(const char*ext_symbol, zorro_asset* asset)
{
	char work[128];
	strcpy(work, ext_symbol);
	// TODO: split extended symbol
	// Source:Root - Type - Exchange - Currency("IB:AAPL-STK-SMART-USD")
	if (!strtok(work, ":")) {
		showMsg("Source not supported for assets in this plugin. change config!");
		exit(-656);
	}
	char *tok = strtok(work, "-");  // not reentrant! should lock
	if (!tok) {
		strcpy(asset->root, work); // no tokenization needed.
	} else {
		strcpy(asset->root, tok);
		tok = strtok(NULL, "-");
	}
	if (tok) {
		strcpy(asset->type, tok);
		tok = strtok(NULL, "-");
	}
	if (tok) {
		strcpy(asset->exchange, tok);
		tok = strtok(NULL, "-");
	}
	if (tok) {
		strcpy(asset->currency, tok);
	}
	// root is always filled.
}


/*
* get the current exchange rate for the given currency.
* cached
*/
double get_exchange_rate(const char* dest_ccy) 
{
	if (!dest_ccy || !strlen(G.currency))
		return 0.; // no exchange for this currency.

	exchg_rate* er = NULL;

	HASH_FIND_STR(EXCHG_Rates, dest_ccy, er);

	if (!er) {
		er = (exchg_rate*)malloc(sizeof * er);
		if (!er) {
			showMsg("serious malloc failure, have to exit Zorro");
			exit(errno);
		}
		strcpy(er->currency, dest_ccy); // our key for the hashmap
		char* suburl = strf("/iserver/exchangerate?target=%s&source=%s", dest_ccy, G.currency);
		json_object* jreq = send(suburl);
		if (!jreq) {
			json_object_put(jreq);
			free(er);
			return 0.;
		}
		json_object *rate_obj = json_object_object_get(jreq, "rate");
		er->rate = 1.; // default value
		if (rate_obj) {
			er->rate = json_object_get_double(rate_obj);
		}
		json_object_put(jreq);
		HASH_ADD_STR(EXCHG_Rates, currency, er);
	}

	return er->rate;
}


/**
 send method with error handling.
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
