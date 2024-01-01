//
// Interactive Brokers Client Portal API
//

#include <windows.h>
typedef double DATE;
#include <trading.h>
#include <stdio.h>
#include <zorro.h>
#include <time.h> // for uman readable epoch time
#include <string.h>

#include "uthash.h"
#include "ib-cpapi.h"

#define PLUGIN_TYPE 2
#define PLUGIN_NAME "IB ClientPortal API"
#define PLUGIN_VERSION "1.0"
#define CPAPI_VERSION "2023-12-14"


// we save all our info from Zorro here..
Global G;

// the current Asset from IB, taken from Hashmap
ib_asset* IB_Asset = NULL;

/*
*  initilize our plugin. when it is loaded by zorro
*/
DLLFUNC int BrokerOpen(char* Name, FARPROC fpMessage, FARPROC fpProgress) {
	strcpy_s(Name, 32, PLUGIN_NAME);
	registerCallbacks(fpMessage, fpProgress);
	// erase everything in our global storage
	memset(&G, 0, sizeof(G));
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
		json_object* jreq = send("/iserver/accounts");
		if (!jreq) {
			return 0;
		}
		json_object* obj;
		if (json_object_object_get_ex(jreq, "selectedAccount", &obj))
			strcpy_s(G.account_id, json_object_get_string(obj));

		G.logged_in = 2; // see BrokerTime
		G.wait_time = 60000; // default Wait Time for Orders (SET_WAIT)
		json_object_put(jreq);

		jreq = send("/portfolio/accounts");
		if (!jreq) {
			return 0;
		}
		json_object* accs = json_object_array_get_idx(jreq, 0);

		if (json_object_object_get_ex(accs, "displayName", &obj))
			strcpy_s(G.account_name, json_object_get_string(obj));

		if (json_object_object_get_ex(accs, "type", &obj))
			strcpy_s(G.account_type, json_object_get_string(obj));

		if (json_object_object_get_ex(accs, "currency", &obj))
			strcpy_s(G.currency, json_object_get_string(obj));

		showMsg("ClientPortal-API connected to Account ", G.account_name);
		showMsg(strf("its a %s account in %s", G.account_type, G.currency));
		showMsg(strf("Plugin version %s, API-Version %s", PLUGIN_VERSION, CPAPI_VERSION));
		json_object_put(jreq);
		return 1;
	}
	return 0;
}

/*
*  check Connection to ClientPortal
*  0 = Lost, 1 = market closed, 2 = ok and market open
* -> market time depends on Asset? -> market time depends on Exchange!
*  
* for now: only check if we have a connection to our gateway.
* no extra checking for time
*/
DLLFUNC int BrokerTime(DATE* pTimeGMT)
{
	return G.logged_in;
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
	double* pVolume, double* pPip, double* pPipCost, double* pLotAmount,
	double* pMarginCost, double* pRollLong, double* pRollShort)
{
	int conId = getContractIdForSymbol(symb);
	if (conId <= 0) {
		return 0;
	}
	char* fields = "31,55,86,84";
	if (G.volume_type == 4) {
		fields = "31,55,86,84,7762";
	}
	char* suburl = strf("/iserver/marketdata/snapshot?conids=%d&fields=%s", conId, fields);

	struct json_object* jreq = send(suburl);
	if (!jreq || !json_object_is_type(jreq, json_type_array)) {
		json_object_put(jreq);
		return 0;
	}
	strcpy_s(G.symbol, symb); // save symbol for later usage in contracts (SET_SYMBOL)
	json_object* obj;
	json_object* arrElem = json_object_array_get_idx(jreq, 0);

	if (json_object_object_get_ex(arrElem, "6509", &obj)) {  // MarketData Availability
		if (!strncmp(json_object_get_string(obj), "Z", 1)) {
			G.logged_in = 1;
		}
		if (!strncmp(json_object_get_string(obj), "Y", 1)) {
			G.logged_in = 1;
		}
		if (!strncmp(json_object_get_string(obj), "R", 1)) {
			G.logged_in = 2;
		}
		if (!strncmp(json_object_get_string(obj), "D", 1)) {
			G.logged_in = 2;
		}
	}

	if (json_object_object_get_ex(arrElem, "31", &obj)) {
		if (!strncmp(json_object_get_string(obj), "C", 1)) {
			G.logged_in = 1;
		}
	}


	// ask-Price
	double ask = 0.;
	if (json_object_object_get_ex(arrElem, "86", &obj) && pPrice) {
		ask = json_object_get_double(obj);
		*pPrice = ask;
	}
	if (json_object_object_get_ex(arrElem, "84", &obj) && pSpread) {
		*pSpread = ask- json_object_get_double(obj);
	}
	if (G.volume_type == 4 && json_object_object_get_ex(arrElem, "7762", &obj) && pVolume) {
		*pVolume = json_object_get_double(obj);
	}
	if (pPip) *pPip = IB_Asset->increment;
	if (pLotAmount) *pLotAmount = IB_Asset->size_increment;
	if (pPipCost) {
		*pPipCost = IB_Asset->size_increment * IB_Asset->increment * IB_Asset->exchrate;
	}
	// pMargin -> no API for that
	// pRollLong/Short -> no API for that
	// pComission -> no API for that

	json_object_put(jreq);
	return 1;
}


/*
* called we we want to order some lots of a symbol.
* 
* return 0 if order was rejected or not filled within wait time (SET_WAIT)
* Trade-ID if the order was filled
* 
* WHEN OrderType is FillOrKill or IOC then the order has to be cancelled by the plugin if necessary.
* 
*/
DLLFUNC int BrokerBuy2(char* symb, int amo, double stopDist, double limit, double* pPrice, int* pFill)
{
	int conId = getContractIdForSymbol(symb);
	if (conId <= 0) {
		return 0;
	}
	char *suburl = strf("/iserver/account/%s/orders", G.account_id); 

	// create the json object for the order
	json_object* jord = create_json_order_payload(conId, limit, amo, stopDist);

	json_object* jreq = send(suburl, json_object_to_json_string(jord));
	json_object_put(jord);

	if (!jreq) goto send_error;
	
	json_object* error_obj = json_object_object_get(jreq, "error");
	if (error_obj) {
		showMsg("Cannot execute order:", json_object_get_string(error_obj));
		goto send_error;
	}

	// probably we are asked some questions..
	if (!order_question_answer_loop(&jreq)) goto send_error;

	// now we have a real order-reply
	// sanity-check to be shure
	if (!jreq) goto send_error;

	// order was accepted
	json_object* first_reply = json_object_array_get_idx(jreq, 0);
	json_object* order_id_obj = json_object_object_get(first_reply, "order_id"); // DIFF "key" to docs from IB
	json_object* order_status_obj = json_object_object_get(first_reply, "order_status");
	const char* order_status = json_object_get_string(order_status_obj);

	if (!strcmp(order_status, "submitted")) {
		// strange things happen
		showMsg("Cannot submit order:", order_status);
		goto send_error;
	}

	int order_id = json_object_get_int(order_id_obj);

	// Order is now submitted. No lets check for the orderState and fill
	suburl = strf("/iserver/account/order/status/%d", order_id);
	int wait = G.wait_time / 1000; // check every second
	bool not_complete = true;
	int cum_fill = 0;
	double avg_price = 0.;
	while (not_complete && --wait > 0) {
		jreq = send(suburl);
		if (!jreq) goto send_error;
		// printf("\njobj from str:\n---\n%s\n---\n", json_object_to_json_string_ext(jreq, JSON_C_TO_STRING_SPACED | JSON_C_TO_STRING_PRETTY));
		json_object* ccp_status_obj = json_object_object_get(jreq, "order_ccp_status");
		not_complete = strcmp(json_object_get_string(ccp_status_obj), "2");
		cum_fill = json_object_get_int(json_object_object_get(jreq, "cum_fill"));
		avg_price = json_object_get_double(json_object_object_get(jreq, "average_price"));
		if (!sleep(500, 1)) break;
	}

	if (not_complete) {
		// check orderType if we should cancel the order..
		if (G.order_type & 2 || G.order_type == 0) {
			showMsg("Order not completly filled. Will wait.");
		}
		else {
			debug(strf("ordertype %d", G.order_type));
			showMsg("Order not completly filled. Don't wait. Cancelling order");
			cancel_trade(order_id);
			return 0;
		}
	}

	if (pPrice) *pPrice = avg_price;
	if (pFill) *pFill = cum_fill;

	json_object_put(jreq);
	return order_id;

send_error:
	json_object_put(jreq);
	return 0;

}

/*
*  Get the overall state of our portfolio
* 
*  return 0 if we haveno infos about balance and tradeVolume
*/
DLLFUNC int BrokerAccount(char* g, double* pdBalance, double* pdTradeVal, double* pdMarginVal)
{
	char *suburl = strf("/portfolio/%s/ledger", g ? g : G.account_id); 

	json_object* jreq = send(suburl); 
	if (!jreq) {
		return 0;
	}
	// we want everything in the same base-currency
	json_object* baseLedger = json_object_object_get(jreq, "BASE");
	json_object* obj;

	double bal = 0.;
	double net = 0.;
	if (json_object_object_get_ex(baseLedger, "cashbalance", &obj)) {
		bal = json_object_get_double(obj);
	} else {
		json_object_put(jreq);
		showMsg("no balance in response", suburl);
		return 0;
	}
	if (json_object_object_get_ex(baseLedger, "netliquidationvalue", &obj)) {
		net = json_object_get_double(obj);
	} else {
		json_object_put(jreq);
		showMsg("no networth in response", suburl);
		return 0;
	}

	if (pdBalance) *pdBalance = bal ;
	if (pdTradeVal) *pdTradeVal = net - bal;
	json_object_put(jreq);

	return 1;
}

/*
* gets called when we want to have historical data from our broker.
* if we need more than 300 ticks (which are bars, not ticks)
* then this method gets called a multiple times with different tEnd-Date
* 
* return 0 is something is wrong, number of bars otherwise
*/
DLLFUNC int BrokerHistory2(char* ext_symbol, DATE tStart, DATE tEnd, int nTickMinutes, int nTicks, T6* ticks)
{
	int conId = getContractIdForSymbol(ext_symbol);
	if (conId <= 0) {
		return 0;
	}
	debug(strf("History 2%s - %s\n", strdate(YMDHMS, tStart), strdate("%Y%m%d %H:%M:%S", tEnd)));
	char barParam[15];
	char durParam[15];
	if (nTickMinutes <= 30) {  // less then half an hour per tick
		sprintf(barParam, "%dmin", nTickMinutes);
		sprintf(durParam, "%dh", (nTickMinutes * nTicks) / 60); // we need hours
	}
	else if (nTickMinutes < 1440) { // less then a day per tick
		sprintf(barParam, "%dh", nTickMinutes / 60);  // we need hours
		sprintf(durParam, "%dd", (nTickMinutes * nTicks) / (60 * 24)); // we need days
	}
	else { // at least a day per tick. We should check for complete bar-intervals and reject..
		sprintf(barParam, "%dd", nTickMinutes / (60 * 24));  // we need days
		sprintf(durParam, "%dd", (nTickMinutes * nTicks) / (60 * 24)); // we need days
	}
	int points = 0;
	json_object* jreq = NULL;
	json_object* obj;
	while (points == 0 && tEnd > tStart) {
		char* suburl = strf("/iserver/marketdata/history?conid=%d&bar=%s&period=%s&startTime=%s",
			conId, barParam, durParam, strdate("%Y%m%d-%H:%M:%S", tEnd));
		jreq = send(suburl);
		if (!jreq) {
			return 0;
		}
		if(json_object_object_get_ex(jreq, "points", &obj))
			points = json_object_get_int(obj);
		if (points == 0) {
			showMsg("skipping end-date");
			json_object_put(jreq);
			tEnd--;
		}
	}
	json_object* data_arr = json_object_object_get(jreq, "data");

	for (int i = json_object_array_length(data_arr) - 1; i >= 0; i--)
	{
		json_object* data = json_object_array_get_idx(data_arr, i);

		if (json_object_object_get_ex(data, "o", &obj))
			ticks->fOpen = (float)json_object_get_double(obj);
		if (json_object_object_get_ex(data, "c", &obj))
			ticks->fClose = (float)json_object_get_double(obj);
		if (json_object_object_get_ex(data, "h", &obj))
			ticks->fHigh = (float)json_object_get_double(obj);
		if (json_object_object_get_ex(data, "l", &obj))
			ticks->fLow = (float)json_object_get_double(obj);
		if (G.volume_type == 4 && json_object_object_get_ex(data, "v", &obj)) {
			ticks->fVol = (float)json_object_get_double(obj);
		}
		if (json_object_object_get_ex(data, "t", &obj)) {
			const time_t val = json_object_get_int64(obj);
			ticks->time = convertEpoch2DATE(val);
			// const time_t val2 = val / 1000ull;
			// printf(strf("%llu -> %f, %s", val, convertEpoch2DATE(val),	ctime(&val2) ));
		}
		else {
			showMsg("strange history-replay, no time included, stopping!", json_object_to_json_string_ext(data, 0));
			json_object_put(jreq);
			return 0;
		}
		ticks++;
	}
	json_object_put(jreq);
	return points;
}

/*
* returning the order fill state for any given (open, partially filled) order
* called when the price moves. maybe it was filled then.
* 
*/
DLLFUNC int BrokerTrade(int nTradeID, double* pOpen, double* pClose, double* pCost, double* pProfit)
{
	ib_trade ibt;

	int ret = fetch_trade(nTradeID, &ibt);

	if (pOpen) *pOpen = ibt.avg_price;

	return ret;
}

/*
*  for communication with our plugin.
*  mostly the infos will be stored in G
*/
DLLFUNC double BrokerCommand(int command, intptr_t parameter)
{
	switch (command) {
	case GET_COMPLIANCE: return 2.; // no hedging
	case GET_MAXREQUESTS: return 10.; // max 10 req/s
	case SET_ORDERTYPE: return G.order_type = parameter;
	case SET_DIAGNOSTICS: G.diag = parameter; return 1;
	case SET_WAIT: return G.wait_time = parameter;
	case SET_VOLTYPE: G.volume_type = parameter; return (parameter == 4);
	case SET_SERVER: strcpy(G.server, (char*)parameter); return 1.; // change localhost
	case GET_ACCOUNT: strcpy((char*)parameter, G.account_id); return 1;
	case SET_SYMBOL: strcpy(G.symbol, (char*)parameter); return 1.;
	case SET_ORDERTEXT: strcpy_s(G.order_text, (char*)parameter); return 1.;
	case GET_TRADES: return get_trades((TRADE*) parameter); 
	case GET_POSITION: return get_position((const char *)parameter); // called with symbol
	case GET_OPTIONS: return get_options((CONTRACT*)parameter);
	case DO_CANCEL: return cancel_trade(parameter); // called with Trade ID
	case SET_COMBO_LEGS: return 0; // TODO
	}
	return 0.;
}

////////////////////////////////////////////////////////////////////////
//
//   Helper functions
//
////////////////////////////////////////////////////////////////////////

int fetch_trade(int nTradeID, ib_trade *ibt) {
	// Order is now submitted. No lets check for the orderState and fill
	char* suburl = strf("/iserver/account/order/status/%d", nTradeID);

	memset(ibt, 0, sizeof(ib_trade));

	ibt->trade_id = nTradeID;

	json_object* jreq = send(suburl);
	if (!jreq) {
		json_object_put(jreq);
		return NAY;
	}

	json_object* obj;

	if (json_object_object_get_ex(jreq, "conid", &obj)) {
		ibt->contract_id = json_object_get_int(obj);
	}

	if (json_object_object_get_ex(jreq, "side", &obj)) {
		if (strcmp(json_object_get_string(obj), "S")) {
			ibt->flags = TR_SHORT;
		}
	}
	if (json_object_object_get_ex(jreq, "size", &obj)) {
		ibt->quantity = json_object_get_int(obj);
	}
	if (json_object_object_get_ex(jreq, "cum_fill", &obj)) {
		ibt->filled = json_object_get_int(obj);
	}
	if (json_object_object_get_ex(jreq, "average_price", &obj)) {
		ibt->avg_price = json_object_get_double(obj);
	}
	if (json_object_object_get_ex(jreq, "sec_type", &obj)) {
		const char* sectype= json_object_get_string(obj);
		if (!strcmp(sectype, "FUT")) {

		}
		if (!strcmp(sectype, "OPT")) {

		}
		if (!strcmp(sectype, "FOP")) {

		}
	}

	return ibt->filled;
}

int get_options(const CONTRACT* contracts) {
	// TODO
	return 0;
}


int get_position(const char* symbol) {
	const char* suburl = "/iserver/account/orders";

	int contract_id = getContractIdForSymbol(symbol);
	if (contract_id <= 0) {
		return 0;
	}

	json_object* jreq = send(suburl);
	if (!jreq || !json_object_is_type(jreq, json_type_array)) {
		json_object_put(jreq);
		return 0;
	}

	// scan all open orders for our contract-id.
	json_object* obj;
	json_object* orders = json_object_object_get(jreq, "orders");
	int pos_count = json_object_array_length(orders);
	for (int i = 0; i < pos_count; i++)
	{
		if (json_object_object_get_ex(jreq, "conid", &obj)) {
			if (contract_id == json_object_get_int(obj)) {
				if (json_object_object_get_ex(jreq, "totalSize", &obj)) {
					int res = json_object_get_int(obj);
					json_object_put(jreq);
					return res;
				}
			}
		}
	}
	json_object_put(jreq);
	return 0;
}


int cancel_trade(int trade_id)
{
	if (!trade_id) {
		showMsg("cancelling of all trades not supported yet!");
		return 0;
	}
	char* suburl = strf("/iserver/account/%s/order/%d", G.account_id, trade_id);

	json_object* jreq = send(suburl, NULL, "DELETE");
	if (!jreq) {
		json_object_put(jreq);
		return 0;
	}

	json_object* obj;
	if (json_object_object_get_ex(jreq, "error", &obj)) {
		showMsg("cannot delete order", json_object_get_string(obj));
		json_object_put(jreq);
		return 0;
	}

	json_object_put(jreq);
	return 1;
}

/**
* ask the broker for all trades from the given account.
* fills them to the given array
*/
int get_trades(TRADE* trades)
{
	const char* suburl = "/iserver/account/orders";
	ib_trade ibt;

	// we get a lot of json back.
	json_object* jreq = send(suburl);
	if (!jreq || !json_object_is_type(jreq, json_type_array)) {
		json_object_put(jreq);
		return 0;
	}
	json_object* obj;
	json_object* orders = json_object_object_get(jreq, "orders");
	int pos_count = json_object_array_length(orders);
	for (int i = 0; i < pos_count; i++)
	{
		if (json_object_object_get_ex(jreq, "orderId", &obj)) {
			int trade_id = json_object_get_int(obj);
			int ret = fetch_trade(trade_id, &ibt);
			if (ret > 0) {
				trades[i].nID = trade_id;
				trades[i].nLots = ibt.quantity;
				trades[i].flags = ibt.flags;
				trades[i].fEntryPrice = ibt.avg_price;
				trades[i].fStrike = ibt.strike;
				trades[i].tExitDate = ibt.expiryDate;
				trades[i].nContract = ibt.contractType;
				strcpy_s(trades[i].sInfo, ibt.info);
			}
		}
	}
	json_object_put(jreq);

	return pos_count;
}

/*
* sometime the broker asks us silly questions. We will answer yes to all of them.
*/
int order_question_answer_loop(json_object** jreq)
{
	// question-answer-loop
	// do we get a question back??
	json_object* reply_id_obj = json_object_object_get(json_object_array_get_idx(*jreq, 0), "id");
	while (reply_id_obj) {
		// answer all questions with "yes"
		char* suburl = strf("/iserver/reply/%s", json_object_get_string(reply_id_obj));
		json_object_put(*jreq); // free the old reply-question
		*jreq = send(suburl, "{\"confirmed\":true}");
		if (!*jreq || !json_object_is_type(*jreq, json_type_array)) {
			json_object_put(*jreq);
			return 0;
		}
		reply_id_obj = json_object_object_get(json_object_array_get_idx(*jreq, 0), "id");
	}
	return 1;
}

/*
* create a json-doc for placing an order
*/
json_object* create_json_order_payload(int conId, double limit, int amo, double stopDist)
{
	json_object* jord = json_object_new_object();
	json_object* jord_arr = json_object_new_array();

	json_object* order = json_object_new_object();

	json_object_object_add(order, "conid", json_object_new_int(conId));
	// check ordertype which is set by SET_ORDERTYPE
	// ordertype is known as tif  with IB

	json_object_object_add(order, "orderType", json_object_new_string((limit > .0) ? "LMT" : "MKT"));
	if (limit > .0) {
		json_object_object_add(order, "price", json_object_new_double(limit));
	}
	json_object_object_add(order, "side", json_object_new_string((amo < 0) ? "SELL" : "BUY"));
	switch (G.order_type) {
		// OrderType IOC Immediate Or Cancel  // AON  All Or None ( no partial fills) and no wait
		// AON - Flag is not available on API
	case 1: json_object_object_add(order, "tif", json_object_new_string("FOK")); break;
		// OrderType GTC (order may wait, partially fills are ok)
	case 2: json_object_object_add(order, "tif", json_object_new_string("GTC")); break;
		// OrderType FOK = GTC + AON (wait and complete)
		// not available in API, use GTC
	case 3: json_object_object_add(order, "tif", json_object_new_string("GTC")); break;
		// OrderType DAY
	case 4: json_object_object_add(order, "tif", json_object_new_string("DAY")); break;
		// OrderType OPG // Market on Open
	case 5: json_object_object_add(order, "tif", json_object_new_string("OPG")); break;
		// OrderType CLS // Market on Close
		// not available in API
	case 6:
		// not set,we don't want to wait, we use IOC, but not on every asset..
	default: json_object_object_add(order, "tif", json_object_new_string("DAY")); break;
	}
	json_object_object_add(order, "quantity", json_object_new_int(labs(amo)));

	if (strlen(G.order_text)) {
		json_object_object_add(order, "referrer", json_object_new_string(G.order_text));
	}

	json_object_array_add(jord_arr, order);
	json_object_object_add(jord, "orders", jord_arr);
	return jord;
}

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
void searchContractIdForSymbol(ib_asset *ibass)
{
	if (!ibass) return; // nothing to do

	char *suburl = strf("/iserver/secdef/search?symbol=%s", ibass->root);

	// we get a lot of json back.

	json_object* jreq = send(suburl);
	if (!jreq || !json_object_is_type(jreq, json_type_array)) {
		json_object_put(jreq);
		return;
	}

	// search inside the json for secType, which we got from parsing the symbol
	// loop over the array we get back
	for (unsigned int i = 0; i < json_object_array_length(jreq); i++)
	{
		json_object* contract = json_object_array_get_idx(jreq, i);

		json_object* con_id_backup = json_object_object_get(contract, "conid");
		int conid = json_object_get_int(con_id_backup);

		json_object* sections = json_object_object_get(contract, "sections");
		// here we have to search the sections for our type
		for (unsigned int j = 0; j < json_object_array_length(sections); j++) {
			json_object* section = json_object_array_get_idx(sections, j);

			json_object* secType = json_object_object_get(section, "secType");
			if (!strlen(ibass->secType) || !strcmp(ibass->secType, json_object_get_string(secType))) {
				// we found our contract.
				ibass->contract_id = conid;
				// does it heave a different conId?
				json_object* con_id_alt = json_object_object_get(section, "conid");
				if (con_id_alt)
					ibass->contract_id = json_object_get_int(con_id_alt);
				// done with this symbol.
				break;
			}
		}
		// found?
		if (ibass->contract_id)
			break;
	}
	json_object_put(jreq);
}

/*
* get some more information from the broker about the given asset.
* important is pip + pip_cost
* 
*/
void fill_asset_info(ib_asset* asset)
{
	if (!asset || !asset->contract_id) return; // nothing to do

	char* suburl = strf("/iserver/contract/%d/info-and-rules", asset->contract_id);

	json_object* jreq = send(suburl);
	if (!jreq) {
		json_object_put(jreq);
		return;
	}
	json_object* obj;
	if (json_object_object_get_ex(jreq, "exchange", &obj)) {
		strcpy(asset->exchange, json_object_get_string(obj));
	}
	if (json_object_object_get_ex(jreq, "currency", &obj)) {
		strcpy(asset->currency, json_object_get_string(obj));
	}
	json_object* rules = json_object_object_get(jreq, "rules");
	if (json_object_object_get_ex(rules, "sizeIncrement", &obj)) {
		asset->size_increment = json_object_get_double(obj);
	}
	if (json_object_object_get_ex(rules, "increment", &obj)) {
		asset->increment = json_object_get_double(obj);
	}

	asset->exchrate = get_exchange_rate(asset->currency);

	// compute pip_cost
	// = LotAmount * Pip * Exchange-Rate
	asset->pip_cost = asset->size_increment * asset->increment * asset->exchrate;

	// Market-Hours?
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
int getContractIdForSymbol(const char * ext_symbol)
{
	if (!ext_symbol)
		return 0; // disable trading for null - symbol

	zorro_asset za;
	decompose_zorro_asset(ext_symbol, &za);

	ib_asset* ass = NULL;

	HASH_FIND_STR(IB_Asset, ext_symbol, ass);

	if (!ass) {
		ass = (ib_asset*)calloc(1, sizeof(* ass));
		if (!ass) {
			showMsg("serious malloc failure, have to exit Zorro");
			exit(errno);
		}
		strcpy(ass->symbol, ext_symbol); // our key for the hashmap
		strcpy(ass->root, za.root); 
		strcpy(ass->secType, za.type);
		strcpy(ass->exchange, za.exchange);
		strcpy(ass->currency, za.currency);
		searchContractIdForSymbol(ass);
		if (!ass->contract_id) {
			showMsg(strf("no contract for asset %s found.", ext_symbol));
			free(ass);
			return 0;
		}
		fill_asset_info(ass);
		HASH_ADD_STR(IB_Asset, symbol, ass);
	}

	return ass->contract_id;
}
