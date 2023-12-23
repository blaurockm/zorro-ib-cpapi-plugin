#pragma once

json_object * create_json_order_payload(int conId, double limit, int amo, double stopDist);

int order_question_answer_loop(json_object*& jreq);
