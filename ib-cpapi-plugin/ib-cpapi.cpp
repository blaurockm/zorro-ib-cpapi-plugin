//
// Interactive Brokers Client Portal API
//

#include <windows.h>
typedef double DATE;
#include <trading.h>
#include <stdio.h>
#include <zorro.h>

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
	char AccountId[16]; // account currency
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

/**
 simple send method with error handling.
 chan is for selecting the max return buffer size., must be (1,0) or 2
*/
char* send(const char* suburl, int chan = 0, const char* meth = NULL, const char* bod = NULL)
{
	// a place for the final url and the respone from the server
	static char Url[1024], Buffer1[1024 * 2024], Buffer2[8192], Header[2048];
	// wait 30 seconds for the server to reply
	int size = 0, wait = 3000;
	sprintf_s(Url, "%s%s", BASEURL, suburl);
	strcpy_s(Header, "Content-Type: application/json\n");
	strcpy_s(Header, "User-Agent: Console\n"); // without this we get 403
	// if we add accept-Type then we cannot POST json-body i
	char* resp = chan & 2 ? Buffer2 : Buffer1;
	int maxSize = chan & 2 ? sizeof(Buffer2) : sizeof(Buffer1);
	int reqId = http_request(Url, bod, Header, meth);
	if (!reqId) 
		goto send_error;
	while (!(size = http_status(reqId)) && --wait > 0) {
		if (!sleep(10)) goto send_error;
	}
	if (!size) goto send_error;
	if (!http_result(reqId, resp, maxSize)) goto send_error;
	resp[maxSize - 1] = 0; // prevent buffer overrun
	http_free(reqId);
	return resp;
		
send_error:
	if (reqId) http_free(reqId);
	return NULL;
}

static char gSymbol[256] = "";	// by SET_SYMBOL
static int gOrderType = 0;	// last order type, FOK/GTC
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
		char *resp = send("/iserver/accounts");
		// parsing the request
		struct json_object *jreq = json_tokener_parse(resp);
		if (!jreq) {
			showMsg("error connecing: ", resp);
			return 0;
		}
		struct json_object* selAcc = json_object_object_get(jreq, "selectedAccount");
		memset(&G, 0, sizeof(G));
		strcpy_s(G.Key, json_object_get_string(selAcc));
		showMsg("ClientPortal-API connected to ", G.Key);
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
	// TODO: split extended symbol
	const char * root = "XAUUSD";
	const char * type = "CFD";
	sprintf_s(suburl, "/iserver/secdef/search?symbol=%s", root);

	strcpy(res->symbol, ext_symbol); // our key for the hashmap
	res->contract_id = 0; // marker if found

	// we get a lot of json back.
	char* resp = send(suburl, 1);

	struct json_object* jreq = json_tokener_parse(resp);
	if (!jreq || !json_object_is_type(jreq, json_type_array)) {
		json_object_put(jreq);
		showMsg("error searching contract: ", resp);
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
	char suburl[1024];
	int conId = getContractIdForSymbol(symb);
	if (conId <= 0) {
		showMsg("symbol not found within IB : ", symb);
		return 0;
	}
	sprintf_s(suburl, "/iserver/marketdata/snapshot?conids=%d&fields=31,55,86,84", conId);

	char* resp = send(suburl,1);

	struct json_object* jreq = json_tokener_parse(resp);
	if (!jreq || !json_object_is_type(jreq, json_type_array)) {
		json_object_put(jreq);
		showMsg("error getting market data: ", resp);
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
	char suburl[1024];
	int conId = getContractIdForSymbol(symb);
	if (conId <= 0) {
		showMsg("symbol not found within IB : ", symb);
		return 0;
	}
	sprintf_s(suburl, "/iserver/account/%s/orders", G.Key); // the account we selected during login

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

	char* resp = send(suburl, 1, "POST", json_object_to_json_string(jord));
	json_object_put(jord);

	// showMsg("buy response: ", resp);

	// do we get a question back??
	struct json_object* jreq = json_tokener_parse(resp);
	if (!jreq || !json_object_is_type(jreq, json_type_array)) {
		json_object_put(jreq);
		showMsg("error posting order ", resp);
		return 0;
	}

	// question-answer-loop
	struct json_object* reply_id_obj = json_object_object_get(json_object_array_get_idx(jreq, 0), "id");
	while (reply_id_obj) {
		sprintf_s(suburl, "/iserver/reply/%s", json_object_get_string(reply_id_obj));
		json_object_put(jreq); // free the old reply-question
		resp = send(suburl, 1, "POST", "{\"confirmed\":true}");
		jreq = json_tokener_parse(resp);
		if (!jreq || !json_object_is_type(jreq, json_type_array)) {
			json_object_put(jreq);
			showMsg("error replying to question ", resp);
			return 0;
		}
		reply_id_obj = json_object_object_get(json_object_array_get_idx(jreq, 0), "id");
	}

	// now we have a real order-reply
	// sanity-check to be shure
	if (!jreq || !json_object_is_type(jreq, json_type_array)) {
		json_object_put(jreq);
		showMsg("error order  ", resp);
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
	char suburl[1024];
	sprintf_s(suburl, "/portfolio/%s/ledger", g ? g : G.Key); // the account we selected during login or the given one

	char* resp = send(suburl, 1);

	struct json_object* jreq = json_tokener_parse(resp);
	if (!jreq) {
		json_object_put(jreq);
		showMsg("error getting account ledger: ", resp);
		return 0;
	}
	struct json_object* baseLedger = json_object_object_get(jreq, "BASE");

	// ask-Price
	struct json_object* balance = json_object_object_get(baseLedger, "cashbalance");
	if (!balance) {
		json_object_put(jreq);
		showMsg("no balance in response", resp);
		return 0;
	}
	double bal = json_object_get_double(balance);
	struct json_object* networth = json_object_object_get(baseLedger, "netliquidationvalue");
	if (!networth) {
		json_object_put(jreq);
		showMsg("no networth in response", resp);
		return 0;
	}
	double net = json_object_get_double(networth);

	if (pdBalance) *pdBalance = bal ;
	if (pdTradeVal) *pdTradeVal = net - bal;
	json_object_put(jreq);

	return 1;
}


DLLFUNC int BrokerHistory2(char* ass, DATE tStart, DATE tEnd, int nTickMinutes, int nTicks, T6* ticks)
{
	for (int i = 0; i < nTicks; i++) {
		ticks->fOpen = 1.01F;
		ticks->fClose = 1.02F;
		ticks->fHigh = 1.0005 * max(ticks->fOpen, ticks->fClose);
		ticks->fLow = 0.9995 * min(ticks->fOpen, ticks->fClose);
		ticks->time = tEnd - i * nTickMinutes / 1440.;
		ticks++;
	}
	return nTicks;
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

DLLFUNC double BrokerCommand(int command, intptr_t parameter)
{
	switch (command) {
	case GET_COMPLIANCE: return 2.;
	case GET_MAXREQUESTS: return 30.;
	case SET_ORDERTYPE: return gOrderType = parameter;
	case SET_SYMBOL: strcpy_s(gSymbol, (char*)parameter); return 1.;
	}
	return 0.;
}
