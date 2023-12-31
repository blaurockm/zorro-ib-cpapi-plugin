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

// helper method for decompose
void fill_element(const char* buf, int element, zorro_asset* asset)
{
	switch (element)
	{
	case 0: strcpy(asset->root, urlsanitize(buf)); break;
	case 1: strcpy(asset->type, buf); break;
	case 2:
		// depends on type, FUT, OPT and FOP do have different elements
		// FUT 2 = Expiry, 3 = Class, 4 = exchange, 5=currency
		// OPT 2 = Expiry, 3 = Strike, 4 = P/C, 5= exchange, 6= currency
		// FOP 2 = Expiry, 3 = Strike, 4 = P/C, 5= class, 6= exchange
		if (strstr(asset->type, "FUTOPTFOP")) {
			strcpy(asset->expiry, buf); break;
		}
		else {
			strcpy(asset->exchange, buf); break;
		}
	case 3: if (strstr(asset->type, "FUTOPTFOP")) {
				if (!strcmp(asset->type, SECTYPE_FUT)) {
					strcpy(asset->tclass, buf); break;
				}
				strcpy(asset->strike, buf); break;
			}
			strcpy(asset->currency, buf); break;
	case 4:	if (strstr(asset->type, "FUTOPTFOP")) {
				if (!strcmp(asset->type, SECTYPE_FUT)) {
					strcpy(asset->exchange, buf); break;
				}
				strcpy(asset->put_or_call, buf);
			}
			break;
	case 5:	if (!strcmp(asset->type, SECTYPE_FUT)) {
				strcpy(asset->currency, buf); break;
			}
			if (!strcmp(asset->type, SECTYPE_OPT)) {
				strcpy(asset->exchange, buf); break;
			}
			if (!strcmp(asset->type, SECTYPE_FOP)) {
				strcpy(asset->tclass, buf);
			}
			break;
	case 6:	if (!strcmp(asset->type, SECTYPE_OPT)) {
				strcpy(asset->currency, buf); break;
			}
			if (!strcmp(asset->type, SECTYPE_FOP)) {
				strcpy(asset->exchange, buf); break;
			}
			break;
	}
}

/*
* fill the given struct with the info parsed from the asset
*/
void decompose_zorro_asset(const char* ext_symbol, zorro_asset* asset)
{
	char work[256];
	strcpy(work, ext_symbol);
	// TODO: split extended symbol
	// Source:Root - Type - Exchange - Currency  for STK, CFD, CMDTY, CASH
	//  -> ("IB:AAPL-STK-SMART-USD")
	// Root - Type - Expiry - Class - Exchange - Currency for FUT
	//  -> ("SPY-FUT-20231218-SPY1C-GLOBEX-USD")
	// Root - Type - Expiry - Strike - P/C - Exchange - Currency  for OPT
	//  -> ("AAPL-OPT-20191218-1350.0-C-GLOBEX-USD")
	// Root - Type - Expiry - Strike - P/C - Class - Exchange for FOP
	//  -> ("ZS-FOP-20191218-900.0-C-OSD-ECBOT")
	// P/C means Put or Call
	// missing Types WAR (warrant) and BOND
	if (!strtok(work, ":")) {
		showMsg("Source not supported for assets in this plugin. change config!");
		exit(-656);
	}
	char buf[128];
	int pos = 0;
	int element = 0;
	for (unsigned int i = 0; i < strlen(work); i++) {
		if (work[i] == '-') {
			// we have a new element
			buf[pos] = '\0';
			fill_element(buf, element++, asset);
			pos = 0; // start over
			buf[pos] = '\0';
		}
		else {
			buf[pos++] = work[i];
		}
	}
	// there is no trainling -, do the last one
	buf[pos] = '\0';
	fill_element(buf, element++, asset);
}



/*
* get the current exchange rate for the given currency.
* cached
*/
double get_exchange_rate(const char* dest_ccy) 
{
	if (!dest_ccy || !strlen(G.currency) || !strlen(dest_ccy))
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
		char* suburl = strf("/iserver/exchangerate?target=%s&source=%s", G.currency, dest_ccy);
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
		showMsg(strf("one %s costs %f %s", dest_ccy, er->rate, G.currency));
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
	json_object* jresp = NULL;
	debug(suburl);
	sprintf_s(url, "%s%s", BASEURL, suburl);
	strcpy_s(header, "Content-Type: application/json\n");
	strcpy_s(header, "User-Agent: Console\n"); // without this we get 403
	// if we add accept-Type then we cannot POST json-body 
	// method is only used when body is given. otherwise its always GET
	int reqId = http_request(url, bod, header, (bod ? (meth ? meth : "POST") : NULL));
	if (!reqId) {
		G.logged_in = 0; // no connection to our gateway
		goto send_error;
	}
	while (!(size = http_status(reqId)) && --wait > 0) {
		if (!sleep(10)) goto send_error;
	}
	if (!size) goto send_error;
	if (!http_result(reqId, resp, sizeof(resp))) goto send_error;
	resp[sizeof(resp) - 1] = 0; // prevent buffer overrun
	debug(resp);
	http_free(reqId);
	if (strlen(resp) < 5 || !strncmp(resp, "ERROR", 5)) {
		showMsg("error with request ", strcatf(resp, suburl));
		return NULL;
	}
	jresp = json_tokener_parse(resp);
	// debug(json_object_to_json_string_ext(jresp,0)); already done with resp above..
	return jresp;

send_error:
	if (reqId) http_free(reqId);
	debug("no response from API");
	return NULL;
}

void debug(const char* msg)
{
	if (G.diag) BrokerMessage(msg);
}

void debug(json_object* json)
{
	if (G.diag) BrokerMessage(json_object_to_json_string(json));
}

/*
* removes or encodes all characters from string which are not allowed
* in URLs. 
* eg: EUR/USD will become "EURUSD"
*/
char* urlsanitize(const char* param)
{
	// allocate memory for the worst possible case (all characters need to be encoded)
	char* encodedText = (char*)malloc(sizeof(char) * strlen(param) * 3 + 1);
	if (!encodedText) {
		showMsg("serious malloc failure, have to exit Zorro");
		exit(errno);
	}

	const char* hex = "0123456789abcdef";

	int pos = 0;
	for (unsigned int i = 0; i < strlen(param); i++) {
		if (('a' <= param[i] && param[i] <= 'z')
			|| ('A' <= param[i] && param[i] <= 'Z')
			|| ('0' <= param[i] && param[i] <= '9')) {
			encodedText[pos++] = param[i];
		}
		else {
			// ATM '.' is the only character we will encode, all others get removed..
			if ('.' == param[i]) {
				encodedText[pos++] = '%';
				encodedText[pos++] = hex[param[i] >> 4];
				encodedText[pos++] = hex[param[i] & 15];
			}
		}
	}
	encodedText[pos] = '\0';
	return encodedText;
}