#pragma once

#include <json-c/json.h>
#include "uthash.h"

#define BASEURL "https://localhost:5000/v1/api"


typedef struct GLOBAL {
	int diag; // should be bool
	int PriceType;
	int volume_type;
	int order_type; // Flags with GTC, AllOrNothing, FillOrKill
	char order_text[255]; // SET ORDER_TEXT
	char symbol[64]; // SET_SYMBOL
	char server[256];
	char account_id[16]; // account-identifier
	char account_name[50]; // account Description
	char account_type[16]; // Type of Account
	char currency[10]; // the currency the account is based on
	int logged_in; // reseted after connection-failure.
	int wait_time; // time in ms
} Global; 

typedef struct ib_asset {
	char symbol[128];  // The Key:  extended symbol from Zorro
	int contract_id;
	char subscr[24];
	char root[42];   // symbol from broker
	char secType[15]; // requested security type
	char currency[10];  // the currency the assets trades in
	char exchange[50]; // the exchange this asset is traded on
	char market[100]; // format ZZZ:HHMM-HHMM
	double increment;  // PIP
	double size_increment; // LotAmount
	int multiplier; // for futures / options
	double exchrate; // current exchange rate (not updated)
	double margin_cost; // not available from api - only after trade
	double commission; // not avaiable from api - only after trade
	double roll_long; // interest rate, not available with API (when AccoutCcy != AssetCcy)
	double roll_short; // interest rate, not available with API
	double pip_cost;  // computed
	UT_hash_handle hh;
} ib_asset;

typedef struct zorro_asset {
	char root[32];
	char type[32];
	char exchange[32];
	char currency[16];
} zorro_asset;

typedef struct exchg_rate {
	char currency[10];  // The Key: the currency we want to buy with AccountCurrency
	double rate;
	UT_hash_handle hh;
} exchg_rate;

// Common utility functions
void registerCallbacks(FARPROC fpMessage, FARPROC fpProgress);

void showMsg(const char* text, const char* detail = NULL);

int sleep(int ms, int par=0);

DATE convertTime2DATE(__time32_t t32);

DATE convertEpoch2DATE(long long t32);

__time32_t convertDATE2Time(DATE date);

void decompose_zorro_asset(const char* ext_symbol, zorro_asset* asset);

json_object* send(const char* suburl, const char* bod = NULL, const char* meth = NULL);

void debug(const char* msg);

// broker - business methods

int cancel_trade(int trade_id);

int get_trades(TRADE* trades);

int order_question_answer_loop(json_object*& jreq);

json_object* create_json_order_payload(int conId, double limit, int amo, double stopDist);

int getContractIdForSymbol(char* ext_symbol);

double get_exchange_rate(const char* dest_ccy);
