#include "ft8_country.h"

#include <ctype.h>
#include <string.h>

namespace ft8 {

namespace {

struct Prefix {
    const char *prefix;
    const char *name;
};

// Checked first, longest match wins
const Prefix kExceptions[] = {
    {"BM", "Taiwan"},        {"BN", "Taiwan"},        {"BO", "Taiwan"},        {"BP", "Taiwan"},
    {"BQ", "Taiwan"},        {"BU", "Taiwan"},        {"BV", "Taiwan"},        {"BW", "Taiwan"},
    {"BX", "Taiwan"},        {"CT3", "Madeira"},      {"CU", "Azores"},        {"EA6", "Balearic Is."},
    {"EA8", "Canary Is."},   {"EA9", "Ceuta/Melilla"}, {"FG", "Guadeloupe"},   {"FK", "New Caledonia"},
    {"FM", "Martinique"},    {"FO", "Fr. Polynesia"}, {"FR", "Reunion"},       {"FY", "Fr. Guiana"},
    {"HB0", "Liechtenstein"}, {"IS", "Sardinia"},     {"IT9", "Sicily"},       {"JW", "Svalbard"},
    {"JX", "Jan Mayen"},     {"KH2", "Guam"},         {"KH6", "Hawaii"},       {"KH7", "Hawaii"},
    {"KL", "Alaska"},        {"KP2", "US Virgin Is."}, {"KP3", "Puerto Rico"}, {"KP4", "Puerto Rico"},
    {"AH6", "Hawaii"},       {"AL", "Alaska"},        {"NH6", "Hawaii"},       {"NL", "Alaska"},
    {"NP4", "Puerto Rico"},  {"WH6", "Hawaii"},       {"WL", "Alaska"},        {"WP4", "Puerto Rico"},
    {"OH0", "Aland Is."},    {"OX", "Greenland"},     {"OY", "Faroe Is."},     {"SV5", "Dodecanese"},
    {"SV9", "Crete"},        {"TK", "Corsica"},       {"VP8", "Falkland Is."}, {"VP9", "Bermuda"},
    {"XX9", "Macao"},        {"ZC4", "UK Bases Cyprus"}, {"ZD7", "St. Helena"}, {"4U", "United Nations"},
};

struct Range {
    char lo[3];
    char hi[3];
    const char *name;
};

// First two characters of the callsign, in ASCII order (digits sort before letters)
const Range kRanges[] = {
    {"3A", "3A", "Monaco"},       {"3B", "3B", "Mauritius"},    {"3V", "3V", "Tunisia"},      {"3W", "3W", "Vietnam"},
    {"3Z", "3Z", "Poland"},       {"4A", "4C", "Mexico"},       {"4J", "4K", "Azerbaijan"},   {"4L", "4L", "Georgia"},
    {"4M", "4M", "Venezuela"},    {"4O", "4O", "Montenegro"},   {"4S", "4S", "Sri Lanka"},    {"4X", "4X", "Israel"},
    {"4Z", "4Z", "Israel"},       {"5B", "5B", "Cyprus"},       {"5H", "5H", "Tanzania"},     {"5N", "5N", "Nigeria"},
    {"5R", "5R", "Madagascar"},   {"5T", "5T", "Mauritania"},   {"5W", "5W", "Samoa"},        {"5Z", "5Z", "Kenya"},
    {"6D", "6J", "Mexico"},       {"6W", "6W", "Senegal"},      {"6Y", "6Y", "Jamaica"},      {"7J", "7N", "Japan"},
    {"7X", "7X", "Algeria"},      {"7Z", "7Z", "Saudi Arabia"}, {"8J", "8N", "Japan"},        {"8P", "8P", "Barbados"},
    {"8Q", "8Q", "Maldives"},     {"8R", "8R", "Guyana"},       {"9A", "9A", "Croatia"},      {"9G", "9G", "Ghana"},
    {"9H", "9H", "Malta"},        {"9J", "9J", "Zambia"},       {"9K", "9K", "Kuwait"},       {"9M", "9M", "Malaysia"},
    {"9N", "9N", "Nepal"},        {"9V", "9V", "Singapore"},    {"9Y", "9Z", "Trinidad"},     {"A2", "A2", "Botswana"},
    {"A4", "A4", "Oman"},         {"A6", "A6", "UAE"},          {"A7", "A7", "Qatar"},        {"A9", "A9", "Bahrain"},
    {"AA", "AL", "USA"},          {"AP", "AS", "Pakistan"},     {"AX", "AX", "Australia"},    {"AY", "AZ", "Argentina"},
    {"B0", "BZ", "China"},        {"C3", "C3", "Andorra"},      {"C6", "C6", "Bahamas"},      {"CA", "CE", "Chile"},
    {"CF", "CK", "Canada"},       {"CM", "CM", "Cuba"},         {"CN", "CN", "Morocco"},      {"CO", "CO", "Cuba"},
    {"CP", "CP", "Bolivia"},      {"CQ", "CT", "Portugal"},     {"CV", "CX", "Uruguay"},      {"D2", "D3", "Angola"},
    {"D4", "D4", "Cape Verde"},   {"D7", "D9", "South Korea"},  {"DA", "DR", "Germany"},      {"DS", "DT", "South Korea"},
    {"DU", "DZ", "Philippines"},  {"E2", "E2", "Thailand"},     {"E7", "E7", "Bosnia-Herz."}, {"EA", "EH", "Spain"},
    {"EI", "EJ", "Ireland"},      {"EK", "EK", "Armenia"},      {"EL", "EL", "Liberia"},      {"EM", "EO", "Ukraine"},
    {"EP", "EQ", "Iran"},         {"ER", "ER", "Moldova"},      {"ES", "ES", "Estonia"},      {"ET", "ET", "Ethiopia"},
    {"EU", "EW", "Belarus"},      {"EX", "EX", "Kyrgyzstan"},   {"EY", "EY", "Tajikistan"},   {"EZ", "EZ", "Turkmenistan"},
    {"F0", "FZ", "France"},       {"HA", "HA", "Hungary"},      {"HB", "HB", "Switzerland"},  {"HC", "HD", "Ecuador"},
    {"HE", "HE", "Switzerland"},  {"HF", "HF", "Poland"},       {"HG", "HG", "Hungary"},      {"HH", "HH", "Haiti"},
    {"HI", "HI", "Dominican Rep."}, {"HJ", "HK", "Colombia"},   {"HL", "HL", "South Korea"},  {"HO", "HP", "Panama"},
    {"HQ", "HR", "Honduras"},     {"HS", "HS", "Thailand"},     {"HV", "HV", "Vatican"},      {"HZ", "HZ", "Saudi Arabia"},
    {"I0", "IZ", "Italy"},        {"JA", "JS", "Japan"},        {"JT", "JV", "Mongolia"},     {"JY", "JY", "Jordan"},
    {"K0", "KZ", "USA"},          {"LA", "LN", "Norway"},       {"LO", "LW", "Argentina"},    {"LX", "LX", "Luxembourg"},
    {"LY", "LY", "Lithuania"},    {"LZ", "LZ", "Bulgaria"},     {"N0", "NZ", "USA"},          {"OA", "OC", "Peru"},
    {"OD", "OD", "Lebanon"},      {"OE", "OE", "Austria"},      {"OF", "OJ", "Finland"},      {"OK", "OL", "Czechia"},
    {"OM", "OM", "Slovakia"},     {"ON", "OT", "Belgium"},      {"OU", "OZ", "Denmark"},      {"PA", "PI", "Netherlands"},
    {"PJ", "PJ", "Dutch Caribbean"}, {"PP", "PY", "Brazil"},    {"PZ", "PZ", "Suriname"},     {"S5", "S5", "Slovenia"},
    {"S9", "S9", "Sao Tome"},     {"SA", "SM", "Sweden"},       {"SN", "SR", "Poland"},       {"ST", "ST", "Sudan"},
    {"SU", "SU", "Egypt"},        {"SV", "SZ", "Greece"},       {"T7", "T7", "San Marino"},   {"TA", "TC", "Turkey"},
    {"TF", "TF", "Iceland"},      {"TG", "TG", "Guatemala"},    {"TI", "TI", "Costa Rica"},   {"TM", "TM", "France"},
    {"TR", "TR", "Gabon"},        {"TU", "TU", "Ivory Coast"},  {"TZ", "TZ", "Mali"},         {"UJ", "UM", "Uzbekistan"},
    {"UN", "UQ", "Kazakhstan"},   {"UR", "UZ", "Ukraine"},      {"V3", "V3", "Belize"},       {"V5", "V5", "Namibia"},
    {"VA", "VG", "Canada"},       {"VH", "VN", "Australia"},    {"VO", "VO", "Canada"},       {"VR", "VR", "Hong Kong"},
    {"VU", "VW", "India"},        {"VY", "VY", "Canada"},       {"W0", "WZ", "USA"},          {"XA", "XI", "Mexico"},
    {"XJ", "XO", "Canada"},       {"XU", "XU", "Cambodia"},     {"XV", "XV", "Vietnam"},      {"XW", "XW", "Laos"},
    {"XY", "XZ", "Myanmar"},      {"YA", "YA", "Afghanistan"},  {"YB", "YH", "Indonesia"},    {"YI", "YI", "Iraq"},
    {"YK", "YK", "Syria"},        {"YL", "YL", "Latvia"},       {"YM", "YM", "Turkey"},       {"YN", "YN", "Nicaragua"},
    {"YO", "YR", "Romania"},      {"YS", "YS", "El Salvador"},  {"YT", "YU", "Serbia"},       {"YV", "YY", "Venezuela"},
    {"Z2", "Z2", "Zimbabwe"},     {"Z3", "Z3", "N. Macedonia"}, {"Z6", "Z6", "Kosovo"},       {"ZA", "ZA", "Albania"},
    {"ZB", "ZB", "Gibraltar"},    {"ZF", "ZF", "Cayman Is."},   {"ZL", "ZM", "New Zealand"},  {"ZP", "ZP", "Paraguay"},
    {"ZR", "ZU", "South Africa"}, {"ZV", "ZZ", "Brazil"},
};

const char *unitedKingdom(const char *p)
{
    // G, M and 2 prefixes: the second letter names the country, a digit means England
    switch (p[1]) {
    case 'D': return "Isle of Man";
    case 'I': return "N. Ireland";
    case 'J': return "Jersey";
    case 'M': return "Scotland";
    case 'U': return "Guernsey";
    case 'W': return "Wales";
    default: return "England";
    }
}

const char *russia(const char *p)
{
    // The call area digit separates the European and the Asiatic part
    const char *d = p;
    while (*d && !isdigit((unsigned char)*d)) {
        ++d;
    }
    if (*d == '2' && (d[1] == 'F' || d[1] == 'K')) {
        return "Kaliningrad";
    }
    if (*d == '8' || *d == '9' || *d == '0') {
        return "As. Russia";
    }
    return "Eu. Russia";
}

bool isPortableSuffix(const char *s, size_t len)
{
    if (len == 1) {
        return true;  // /P, /M, /A or a call area digit
    }
    return (len == 2 && (!strncmp(s, "MM", 2) || !strncmp(s, "AM", 2))) || (len == 3 && !strncmp(s, "QRP", 3));
}

}  // namespace

const char *countryForCall(const char *call)
{
    // With a slash, the part that is not the home call names the location: EA8/DL1ABC, DL1ABC/OH0
    char prefix[12];
    const char *best = call;
    size_t best_len = strlen(call);
    const char *part = call;
    bool has_slash = strchr(call, '/') != nullptr;
    if (has_slash) {
        best = nullptr;
        best_len = 0;
        while (*part) {
            const char *end = strchr(part, '/');
            size_t len = end ? (size_t)(end - part) : strlen(part);
            if (len > 0 && !isPortableSuffix(part, len) && (best == nullptr || len < best_len)) {
                best = part;
                best_len = len;
            }
            part += len + (end ? 1 : 0);
        }
        if (best == nullptr) {
            return "";
        }
    }
    if (best_len < 2 || best_len >= sizeof(prefix)) {
        return "";
    }
    for (size_t i = 0; i < best_len; ++i) {
        prefix[i] = (char)toupper((unsigned char)best[i]);
    }
    prefix[best_len] = '\0';

    const char *match = nullptr;
    size_t match_len = 0;
    for (const Prefix &e : kExceptions) {
        size_t len = strlen(e.prefix);
        if (len > match_len && strncmp(prefix, e.prefix, len) == 0) {
            match = e.name;
            match_len = len;
        }
    }
    if (match != nullptr) {
        return match;
    }

    char c0 = prefix[0], c1 = prefix[1];
    if (c0 == 'G' || c0 == 'M' || (c0 == '2' && isalpha((unsigned char)c1))) {
        return unitedKingdom(prefix);
    }
    if (c0 == 'R' || (c0 == 'U' && c1 >= 'A' && c1 <= 'I')) {
        return russia(prefix);
    }
    for (const Range &r : kRanges) {
        if (c0 == r.lo[0] && c1 >= r.lo[1] && c1 <= r.hi[1]) {
            return r.name;
        }
    }
    return "";
}

}  // namespace ft8
