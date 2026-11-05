# RA3 Native

Експериментальний нативний запуск Red Alert 3 на Android (статична рекомпіляція, без Winlator).
Містить лише інструменти. Файли гри не включені і не розповсюджуються. Дивись `docs/PLAN.md`.

`tools/analyze.cpp` — статичний аналіз PE (потрібен Capstone):
`g++ -O2 -std=c++17 -I<capstone>/include tools/analyze.cpp <capstone>/libcapstone.a -o analyze && ./analyze ra3_1.12.game out`
