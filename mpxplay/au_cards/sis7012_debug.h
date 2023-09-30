#pragma once

enum SIS7012_DebugVars {
    SIS7012_DEBUG_IOC_COUNT = 0,
    SIS7012_DEBUG_BUP_COUNT,
    SIS7012_DEBUG_FIFO_COUNT,

    SIS7012_DEBUG_LAST,
};

int sis7012_get(enum SIS7012_DebugVars);
