#pragma once
#include <cstdint>

#pragma pack(1)

// --------------------------------------------------------
// 1.1 System Event Message
// --------------------------------------------------------
struct SystemEvent {            // 'S' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    char     eventcode;
};

// --------------------------------------------------------
// 1.2 Stock Related Messages 
// --------------------------------------------------------
struct StockDirectory {         // 'R'
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    char     stock[8];          // left-justified, space-padded
    char     market_category;
    char     financial_status;
    uint32_t round_lot_size;
    char     round_lots_only;
    char     issue_classification;
    char     issue_subtype[2];
    char     authenticity;
    char     short_sale_threshold;
    char     ipo_flag;
    char     luld_ref_price_tier;
    char     etp_flag;
    uint32_t etp_leverage_factor;
    char     inverse_indicator;
};

struct StockTradingAction {     // 'H' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    char     stock[8];
    char     trading_state;
    char     reserved;
    char     reason[4];
};

struct RegSHORestriction {      // 'Y' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    char     stock[8];
    char     reg_sho_action;
};

struct MarketParticipantPosition { // 'L' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    char     mpid[4];
    char     stock[8];
    char     primary_mm;
    char     mm_mode;
    char     mp_state;
};

struct MWCBDeclineLevel {       // 'V' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    uint64_t level_1;
    uint64_t level_2;
    uint64_t level_3;
};

struct MWCBStatus {             // 'W' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    char     breached_level;
};

struct IPOQuotingPeriodUpdate { // 'K' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    char     stock[8];
    uint32_t release_time;
    char     release_qualifier;
    uint32_t ipo_price;
};

struct LULDAuctionCollar {      // 'J' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    char     stock[8];
    uint32_t ref_price;
    uint32_t upper_price;
    uint32_t lower_price;
    uint32_t extension;
};

struct OperationalHalt {        // 'h' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    char     stock[8];
    char     market_code;
    char     halt_action;
};

// --------------------------------------------------------
// 1.3 Add Order Messages 
// --------------------------------------------------------
struct AddOrder {               // 'A' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    uint64_t order_ref;
    char     side;
    uint32_t shares;
    char     stock[8];
    uint32_t price;
};

struct AddOrderMPID {           // 'F' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    uint64_t order_ref;
    char     side;
    uint32_t shares;
    char     stock[8];
    uint32_t price;
    char     attribution[4];
};

// --------------------------------------------------------
// 1.4 Modify Order Messages 
// --------------------------------------------------------
struct OrderExecuted {          // 'E' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    uint64_t order_ref;
    uint32_t executed_shares;
    uint64_t match_number;
};

struct OrderExecutedWithPrice { // 'C' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    uint64_t order_ref;
    uint32_t executed_shares;
    uint64_t match_number;
    char     printable;
    uint32_t execution_price;
};

struct OrderCancel {            // 'X' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    uint64_t order_ref;
    uint32_t cancelled_shares;
};

struct DeleteOrder {            // 'D' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    uint64_t order_ref;
};

struct OrderReplace {           // 'U' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    uint64_t original_order_ref;
    uint64_t new_order_ref;
    uint32_t shares;
    uint32_t price;
};

// --------------------------------------------------------
// 1.5 Trade Messages 
// --------------------------------------------------------
struct TradeMessage {           // 'P' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    uint64_t order_ref;
    char     side;
    uint32_t shares;
    char     stock[8];
    uint32_t price;
    uint64_t match_number;
};

struct CrossTrade {             // 'Q' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    uint64_t shares;
    char     stock[8];
    uint32_t cross_price;
    uint64_t match_number;
    char     cross_type;
};

struct BrokenTrade {            // 'B' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    uint64_t match_number;
};

// --------------------------------------------------------
// 1.6 & Beyond: Administrative/Imbalance Messages 
// --------------------------------------------------------
struct NOIIMessage {            // 'I' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    uint64_t paired_shares;
    uint64_t imbalance_shares;
    char     imbalance_dir;
    char     stock[8];
    uint32_t far_price;
    uint32_t near_price;
    uint32_t current_ref_price;
    char     cross_type;
    char     price_variation;
};

struct RPII {                   // 'N' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    char     stock[8];
    char     interest_flag;
};

struct DirectListingPriceDiscovery { // 'O' 
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    char     stock[8];
    char     open_eligibility;
    uint32_t min_price;
    uint32_t max_price;
    uint32_t near_exec_price;
    uint64_t near_exec_time;
    uint32_t lower_collar;
    uint32_t upper_collar;
};
#pragma pack()
// Every message carries a 48-bit big-endian timestamp at bytes 5..10:
// nanoseconds since midnight ET.
inline uint64_t itch_ts(const char* msg){
    uint64_t t = 0;
    for(int i = 5; i < 11; i++) t = (t << 8) | (uint8_t)msg[i];
    return t;
}
