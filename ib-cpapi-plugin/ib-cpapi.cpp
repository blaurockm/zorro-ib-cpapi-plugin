//
// Interactive Brokers Client Portal API
//

#include <windows.h>
typedef double DATE;
#include <trading.h>
#include <stdio.h>
#include <zorro.h>
#include <time.h> // for uman readable epoch time

#include "json-c/json.h"
#include "uthash.h"

#define PLUGIN_TYPE 2
#define PLUGIN_NAME "IB ClientPortal API"


#define BASEURL "https://localhost:5000/v1/api"

// Methods from zorro
int(__cdecl* BrokerMessage)(const char* Text);
int(__cdecl* BrokerProgress)(const int Progress);


struct GLOBAL {
	int Diag;
	int HttpId;
	int PriceType, VolType, OrderType;
	double Unit;
	char Url[256]; // send URL buffer
	char Key[256], Secret[256]; // credentials
	char Symbol[64], Uuid[256]; // last trade, symbol and Uuid
	char AccountId[16]; // account-identifier
} G; // parameter singleton

struct ib_asset {
	char symbol[128];  // extended symbol from Zorro
	int contract_id;
	char subscr[24];
	char root[42];   // symbol from broker
	char secType[15]; // requested security type
	UT_hash_handle hh;
};

struct ib_asset* IB_Asset = NULL;

void showMsg(const char* text, const char* detail = NULL)
{
	if (!BrokerMessage) return;
	if (!detail) detail = "";
	static char msg[1024];
	sprintf_s(msg, "%s %s", text, detail);
	BrokerMessage(msg);
}

int sleep(int ms)
{
	Sleep(ms);
	return BrokerProgress(0);
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
struct json_object *send(const char* suburl, const char* bod = NULL)
{
	// a place for the final url and the respone from the server
	static char url[1024], resp[1024 * 2024], header[2048];
	// wait 30 seconds for the server to reply
	int size = 0, wait = 3000;
	sprintf_s(url, "%s%s", BASEURL, suburl);
	strcpy_s(header, "Content-Type: application/json\n");
	strcpy_s(header, "User-Agent: Console\n"); // without this we get 403
	// if we add accept-Type then we cannot POST json-body i
	int reqId = http_request(url, bod, header, (bod ? "POST" : NULL));
	if (!reqId) 
		goto send_error;
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

static int gAmount = 1;	// last order amount

// Global vars ///////////////////////////////////////////////////////////

DLLFUNC int BrokerOpen(char* Name, FARPROC fpMessage, FARPROC fpProgress) {
	strcpy_s(Name, 32, PLUGIN_NAME);
	(FARPROC&)BrokerMessage = fpMessage;
	(FARPROC&)BrokerProgress = fpProgress;
	// we could make an unsubscribe to marketdata here, but this wouzld kill all other running zorros..
	return PLUGIN_TYPE;
}

/*
* calling /iserver/accounts.
* Needed bfore we can call anything from iserver or place orders
* 
*/
DLLFUNC int BrokerLogin(char* User, char* Pwd, char* Type, char* g)
{
	if (User) {
		struct json_object* jreq = send("/iserver/accounts");
		if (!jreq) {
			return 0;
		}
		struct json_object* selAcc = json_object_object_get(jreq, "selectedAccount");
		memset(&G, 0, sizeof(G));
		strcpy_s(G.AccountId, json_object_get_string(selAcc));
		showMsg("ClientPortal-API connected to ", G.AccountId);
		json_object_put(jreq);
		return 1;
	}
	return 0;
}

/*
*  check Connection to ClientPortal
*  0 = Lost, 1 = market closed, 2 = ok and market open
* -> market time depends on Asset!?
* 
*
DLLFUNC int BrokerTime(DATE* pTimeGMT)
{
	char* resp = send("/tickle");
	// parsing the request
	struct json_object* jreq = json_tokener_parse(resp);
	if (!jreq) {
		showMsg("error connecing: ", resp);
		return 0;
	}
	struct json_object* iserver = json_object_object_get(jreq, "iserver");
	struct json_object* authStatus = json_object_object_get(iserver, "authStatus");
	struct json_object* connected = json_object_object_get(authStatus, "connected");
	BOOL conn = json_object_get_boolean(connected);
	json_object_put(jreq);
	if (!conn) {
		showMsg("conection lost");
		return 0;
	}
	return 2;	// broker is online
}
*/

/*
*  security search by (extended-)symbol
*
* find the contract-Id for the given symbol.
* take care for the secType!
*
* extended symbol is multi-in-one-string to search for:

Quote:
> An extended symbol is a text string in the format Source:Root-Type-Exchange-Currency ("IB:AAPL-STK-SMART-USD").
> Assets with an expiration date, such as futures, have the format Root-Type-Expiry-Class-Exchange-Currency ("SPY-FUT-20231218-SPY1C-GLOBEX-USD").
> Stock or index options have the format Root-Type-Expiry-Strike-P/C-Exchange-Currency ("AAPL-OPT-20191218-1350.0-C-GLOBEX-USD").
> Future options have the format Root-Type-Expiry-Strike-P/C-Class-Exchange.("ZS-FOP-20191218-900.0-C-OSD-ECBOT").
> Currencies across multiple brokers or exchanges have the format Currency/Countercurrency-Exchange ("NOK/USD-ISLAND").

* For now we use Root and Type
* TODO: Exchange and Currency
* Ignoring: Source
* 
* Side-effect: create and allocates an ib_asset if found
*/
struct ib_asset* searchContractIdForSymbol(char* ext_symbol)
{
	char suburl[1024];
	// split symbol with "-"
	// 

	struct ib_asset* res;
	res = (struct ib_asset*)malloc(sizeof * res);
	if (!res) {
		showMsg("serious malloc failure, have to exit Zorro");
		exit(errno);
	}
	strcpy(res->symbol, ext_symbol); // our key for the hashmap
	res->contract_id = 0; // marker if found

	// TODO: split extended symbol
	const char * root = "XAUUSD";
	const char * type = "CFD";
	sprintf_s(suburl, "/iserver/secdef/search?symbol=%s", root);

	// we get a lot of json back.

	struct json_object* jreq = send(suburl);
	if (!jreq || !json_object_is_type(jreq, json_type_array)) {
		json_object_put(jreq);
		return NULL;
	}

	// search inside the json for secType, which we got from parsing the symbol
	// loop over the array we get back
	for (unsigned int i = 0; i < json_object_array_length(jreq); i++)
	{
		struct json_object* contract = json_object_array_get_idx(jreq, i);

		struct json_object* con_id_backup = json_object_object_get(contract, "conid");
		int conid = json_object_get_int(con_id_backup);

		struct json_object* sections = json_object_object_get(contract, "sections");
		// here we have to search the sections for our type
		for (unsigned int j = 0; j < json_object_array_length(sections); j++) {
			struct json_object* section = json_object_array_get_idx(sections, j);

			struct json_object* secType = json_object_object_get(section, "secType");
			if (!strcmp(type, json_object_get_string(secType))) {
				// we found our contract.
				res->contract_id = conid;
				// does it heave a different conId?
				struct json_object* con_id_alt = json_object_object_get(section, "conid");
				if (con_id_alt) 
					res->contract_id = json_object_get_int(con_id_alt);
				// done with this symbol.
				break;
			}
		}
		// found?
		if (res->contract_id)
			break;
	}
	json_object_put(jreq);

	return res;
}

/*
* get the IB-contract id of the asset from IB
* 
* we always need this.
* We expect this to be initialized by the first calls of subscribe to Asset
* from Broker_Asset2.
* 
* Keep the contractId in a hashtable for easier lookup.
* 
* return 0 if the symbol is not known to IB, 
* the contractID otherwise.
* 
*/
int getContractIdForSymbol(char* ext_symbol)
{
	if (!ext_symbol)
		return 0; // disable trading for null - symbol

	struct ib_asset* ass = NULL;

	HASH_FIND_STR(IB_Asset, ext_symbol, ass);

	if (!ass) {
		ass = searchContractIdForSymbol(ext_symbol);
		if (!ass || !ass->contract_id)
			return 0;
		HASH_ADD_STR(IB_Asset, symbol, ass);
	}

	return ass->contract_id;
}


/*
* first call subscribes to the market data.
* subsequent-calls retrieve the data
* 
* the serverId contains a unique identifier for the subsciption.
* it will get resetted after "unsubscribeall"
* 
*/
DLLFUNC int BrokerAsset(char* symb, double* pPrice, double* pSpread,
	double* pVolume, double* pPip, double* pPipCost, double* pMinAmount,
	double* pMarginCost, double* pRollLong, double* pRollShort)
{
	int conId = getContractIdForSymbol(symb);
	if (conId <= 0) {
		showMsg("symbol not found within IB : ", symb);
		return 0;
	}
	char* suburl = strf("/iserver/marketdata/snapshot?conids=%d&fields=31,55,86,84", conId);

	struct json_object* jreq = send(suburl);
	if (!jreq || !json_object_is_type(jreq, json_type_array)) {
		json_object_put(jreq);
		return 0;
	}
	struct json_object* arrElem = json_object_array_get_idx(jreq, 0);

	// ask-Price
	struct json_object* field86 = json_object_object_get(arrElem, "86");
	if (!field86) {
		json_object_put(jreq);
		return 1;
	}
	double ask = json_object_get_double(field86);

	// bid-Price
	struct json_object* field84 = json_object_object_get(arrElem, "84");
	double bid = json_object_get_double(field86);

	if (pPrice) *pPrice =ask;
	if (pSpread) *pSpread = ask-bid;
	json_object_put(jreq);
	return 1;
}



DLLFUNC int BrokerBuy2(char* symb, int amo, double StopDist, double limit, double* pPrice, int* pFill)
{
	int conId = getContractIdForSymbol(symb);
	if (conId <= 0) {
		showMsg("symbol not found within IB : ", symb);
		return 0;
	}
	char *suburl = strf("/iserver/account/%s/orders", G.AccountId); 

	// check ordertype which is set by SET_ORDERTYPE
	// ordertype is known as tif  with IB

	// create the json object for the order
	json_object* jord = json_object_new_object();
	json_object* jord_arr = json_object_new_array();

	json_object* order = json_object_new_object();

	json_object_object_add(order, "conid", json_object_new_int(conId));
	json_object_object_add(order, "orderType", json_object_new_string((limit >.0) ? "LMT" : "MKT"));
	if (limit > .0) {
		json_object_object_add(order, "price", json_object_new_double(limit));
	}
	json_object_object_add(order, "side",json_object_new_string((amo < 0) ? "SELL" : "BUY"));
	json_object_object_add(order, "tif", json_object_new_string("GTC"));
	json_object_object_add(order, "quantity", json_object_new_int(labs(amo)));

	json_object_array_add(jord_arr, order);
	json_object_object_add(jord, "orders", jord_arr);

	// showMsg("buy request: ", json_object_to_json_string(jord));

	struct json_object* jreq = send(suburl, json_object_to_json_string(jord));
	json_object_put(jord);

	// showMsg("buy response: ", resp);

	// do we get a question back??
	if (!jreq || !json_object_is_type(jreq, json_type_array)) {
		return 0;
	}

	// question-answer-loop
	struct json_object* reply_id_obj = json_object_object_get(json_object_array_get_idx(jreq, 0), "id");
	while (reply_id_obj) {
		char *suburl = strf("/iserver/reply/%s", json_object_get_string(reply_id_obj));
		json_object_put(jreq); // free the old reply-question
		jreq = send(suburl, "{\"confirmed\":true}");
		if (!jreq || !json_object_is_type(jreq, json_type_array)) {
			json_object_put(jreq);
			return 0;
		}
		reply_id_obj = json_object_object_get(json_object_array_get_idx(jreq, 0), "id");
	}

	// now we have a real order-reply
	// sanity-check to be shure
	if (!jreq || !json_object_is_type(jreq, json_type_array)) {
		json_object_put(jreq);
		return 0;
	}
	struct json_object* first_reply = json_object_array_get_idx(jreq, 0);
	struct json_object* order_id_obj = json_object_object_get(first_reply, "order_id"); // DIFF "key" to docs from IB
	struct json_object* order_status_obj = json_object_object_get(first_reply, "order_status");

	// showMsg("order status ", json_object_get_string(order_status_obj));
	int order_id = json_object_get_int(order_id_obj);
	// showMsg("order id ", strf("%d", order_id));
	json_object_put(jreq);
	return order_id;
}

DLLFUNC int BrokerAccount(char* g, double* pdBalance, double* pdTradeVal, double* pdMarginVal)
{
	char *suburl = strf("/portfolio/%s/ledger", g ? g : G.AccountId); // the account we selected during login or the given one

	struct json_object* jreq = send(suburl); 
	if (!jreq) {
		return 0;
	}
	struct json_object* baseLedger = json_object_object_get(jreq, "BASE");

	// ask-Price
	struct json_object* balance = json_object_object_get(baseLedger, "cashbalance");
	if (!balance) {
		json_object_put(jreq);
		showMsg("no balance in response", suburl);
		return 0;
	}
	double bal = json_object_get_double(balance);
	struct json_object* networth = json_object_object_get(baseLedger, "netliquidationvalue");
	if (!networth) {
		json_object_put(jreq);
		showMsg("no networth in response", suburl);
		return 0;
	}
	double net = json_object_get_double(networth);

	if (pdBalance) *pdBalance = bal ;
	if (pdTradeVal) *pdTradeVal = net - bal;
	json_object_put(jreq);

	return 1;
}


DLLFUNC int BrokerHistory2(char* symb, DATE tStart, DATE tEnd, int nTickMinutes, int nTicks, T6* ticks)
{
	int conId = getContractIdForSymbol(symb);
	if (conId <= 0) {
		showMsg("symbol not found within IB : ", symb);
		return 0;
	}
	showMsg("history2: ", strf("%s - %s\n", strdate(YMDHMS, tStart), strdate("%Y%m%d %H:%M:%S", tEnd)));
	char barParam[15];
	char durParam[15];
	if (nTickMinutes <= 30) {  // less then half an hour per tick
		sprintf(barParam, "%dmin", nTickMinutes);
		sprintf(durParam, "%dh", (nTickMinutes * nTicks) / 60 ); // we need hours
	} else if (nTickMinutes <= 1440) { // less then a day per tick
		sprintf(barParam, "%dh", nTickMinutes / 60);  // we need hours
		sprintf(durParam, "%dd", (nTickMinutes * nTicks) / 60 / 24); // we need days
	}
	char* suburl = strf("/iserver/marketdata/history?conid=%d&bar=%s&period=%s&startTime=%s", 
		conId, barParam, durParam, strdate("%Y%m%d-%H:%M:%S", tEnd));
	struct json_object* jreq = send(suburl);
	if (!jreq) {
		return 0;
	}
	struct json_object* data_arr = json_object_object_get(jreq, "data");
	//printf("\njobj from str:\n---\n%s\n---\n", json_object_to_json_string_ext(data_arr, JSON_C_TO_STRING_SPACED | JSON_C_TO_STRING_PRETTY));

	for (int i = json_object_array_length(data_arr)-1; i>=0 ; i--)
	{
		struct json_object* data = json_object_array_get_idx(data_arr, i); 

		ticks->fOpen = (float) json_object_get_double(json_object_object_get(data,"o"));
		ticks->fClose = (float) json_object_get_double(json_object_object_get(data, "c"));
		ticks->fHigh = (float) json_object_get_double(json_object_object_get(data, "h")); 
		ticks->fLow = (float) json_object_get_double(json_object_object_get(data, "l")); 
		const time_t val = json_object_get_int64(json_object_object_get(data, "t"));
		const time_t val2 = val / 1000ull;
//		printf(strf("%llu -> %f, %s", val,
//			convertEpoch2DATE(val),
//			ctime(&val2)
//			));
		ticks->time = convertEpoch2DATE(val);
		ticks++;
	}
	int points = json_object_get_int(json_object_object_get(jreq, "points"));
	json_object_put(jreq);
	return points;
}


#ifdef BROKERTRADE
DLLFUNC int BrokerTrade(int nTradeID, double* pOpen, double* pClose, double* pCost, double* pProfit)
{
	if (gOrderType == 0)
		return gAmount;
	else // GTC order - stay partially filled forever
		return gAmount;
}
#endif

/**
* ask the broker for all trades from the given account.
* fills them to the given array
*/
int getTrades(TRADE* trades)
{
	return 0;
}


DLLFUNC double BrokerCommand(int command, intptr_t parameter)
{
	switch (command) {
	case GET_COMPLIANCE: return 2.; // no hedging
	case GET_MAXREQUESTS: return 10.; // max 10 req/s
	case SET_ORDERTYPE: return G.OrderType = parameter;
	case SET_SYMBOL: strcpy_s(G.Symbol, (char*)parameter); return 1.;
	case GET_TRADES: return getTrades((TRADE*) parameter);
	}
	return 0.;
}
