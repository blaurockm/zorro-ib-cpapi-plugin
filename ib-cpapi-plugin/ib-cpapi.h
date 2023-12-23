#pragma once

#include <json-c/json.h>
#include "uthash.h"

#define BASEURL "https://localhost:5000/v1/api"


typedef struct GLOBAL {
	int Diag;
	int PriceType, VolType, OrderType;
	double Unit;
	char Url[256]; // send URL buffer
	char Key[256], Secret[256]; // credentials
	char Symbol[64], Uuid[256]; // last trade, symbol and Uuid
	char AccountId[16]; // account-identifier
	int loggedIn;
	int WaitTime; // time in ms
} Global; 

typedef struct ib_asset {
	char symbol[128];  // extended symbol from Zorro
	int contract_id;
	char subscr[24];
	char root[42];   // symbol from broker
	char secType[15]; // requested security type
	UT_hash_handle hh;
} ib_asset;


// Common utility functions
void registerCallbacks(FARPROC fpMessage, FARPROC fpProgress);

void showMsg(const char* text, const char* detail = NULL);

int sleep(int ms, int par=0);

DATE convertTime2DATE(__time32_t t32);

DATE convertEpoch2DATE(long long t32);

__time32_t convertDATE2Time(DATE date);

json_object* send(const char* suburl, const char* bod = NULL, const char* meth = NULL);

// broker - business methods

int getTrades(TRADE* trades);

int order_question_answer_loop(json_object*& jreq);

json_object* create_json_order_payload(int conId, double limit, int amo, double stopDist);

int getContractIdForSymbol(char* ext_symbol);
