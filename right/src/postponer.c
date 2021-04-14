#include "postponer.h"
#include "usb_report_updater.h"
#include "macros.h"
#include "timer.h"

struct postponer_buffer_record_type_t buffer[POSTPONER_BUFFER_SIZE];
static uint8_t bufferSize = 0;
static uint8_t bufferPosition = 0;

static uint8_t cyclesUntilActivation = 0;
static uint32_t lastPressTime;

key_state_t* Postponer_NextEventKey;
uint32_t CurrentPostponedTime = 0;

#define POS(idx) ((bufferPosition + (idx)) % POSTPONER_BUFFER_SIZE)

//##############################
//### Implementation Helpers ###
//##############################

static uint8_t getPendingKeypressIdx(uint8_t n)
{
    for ( int i = 0; i < bufferSize; i++ ) {
        if (buffer[POS(i)].active) {
            if (n == 0) {
                return i;
            } else {
                n--;
            }
        }
    }
    return 255;
}

static void consumeEvent(uint8_t count)
{
    bufferPosition = POS(count);
    bufferSize = count > bufferSize ? 0 : bufferSize - count;
    Postponer_NextEventKey = bufferSize == 0 ? NULL : buffer[bufferPosition].key;
    CurrentPostponedTime = buffer[POS(0-1+POSTPONER_BUFFER_SIZE)].time;
}

//######################
//### Core Functions ###
//######################

// Postpone keys for the next n cycles. If called by multiple callers, maximum of all the
// requests is taken.
//
// 0 means "(rest of) this cycle"
// 1 means "(rest of) this cycle and the next one"
// ...
//
// E.g., if you want to stop key processing for longer time, you want to call
// this with n=1 every update cycle for as long as you want. Once you stop postponing
// the events, Postponer will start replaying them at a pace one every two cycles.
//
// If you just want to perform some action of known length without being disturbed
// (e.g., activation of a key with extra usb reports takes 2 cycles), then you just
// call this once with the required number.
void PostponerCore_PostponeNCycles(uint8_t n)
{
	if(bufferSize == 0 && cyclesUntilActivation == 0) {
        // ensure correct CurrentPostponedTime when postponing starts, since current postponed time is the time of last executed action
        buffer[POS(0-1+POSTPONER_BUFFER_SIZE)].time = CurrentTime;
	}
    cyclesUntilActivation = MAX(n + 1, cyclesUntilActivation);
}

bool PostponerCore_IsActive(void)
{
    return bufferSize > 0 || cyclesUntilActivation > 0;
}


void PostponerCore_TrackKeyEvent(key_state_t *keyState, bool active)
{
    uint8_t pos = POS(bufferSize);
    buffer[pos] = (struct postponer_buffer_record_type_t) {
            .time = CurrentTime,
            .key = keyState,
            .active = active,
    };
    bufferSize = bufferSize < POSTPONER_BUFFER_SIZE ? bufferSize + 1 : bufferSize;
    lastPressTime = active ? CurrentTime : lastPressTime;
}

void PostponerCore_RunPostponedEvents(void)
{
    // Process one event every two cycles. (Unless someone keeps Postponer active by touching cycles_until_activation.)
    if (bufferSize != 0 && (cyclesUntilActivation == 0 || bufferSize > POSTPONER_BUFFER_MAX_FILL)) {
        buffer[bufferPosition].key->current = buffer[bufferPosition].active;
        consumeEvent(1);
        // This gives the key two ticks (this and next) to get properly processed before execution of next queued event.
        PostponerCore_PostponeNCycles(1);
    }
}

void PostponerCore_FinishCycle(void)
{
    cyclesUntilActivation -= cyclesUntilActivation > 0 ? 1 : 0;
    if(bufferSize == 0 && cyclesUntilActivation == 0) {
        CurrentPostponedTime = CurrentTime;
    }
}

//#######################
//### Query Functions ###
//#######################


uint8_t PostponerQuery_PendingKeypressCount()
{
    uint8_t cnt = 0;
    for ( uint8_t i = 0; i < bufferSize; i++ ) {
        if (buffer[POS(i)].active) {
            cnt++;
        }
    }
    return cnt;
}

bool PostponerQuery_IsKeyReleased(key_state_t* key)
{
    if (key == NULL) {
        return false;
    }
    for ( uint8_t i = 0; i < bufferSize; i++ ) {
        if (buffer[POS(i)].key == key && !buffer[POS(i)].active) {
            return true;
        }
    }
    return false;
}

void PostponerQuery_InfoByKeystate(key_state_t* key, struct postponer_buffer_record_type_t** press, struct postponer_buffer_record_type_t** release)
{
    *press = NULL;
    *release = NULL;
    for ( int i = 0; i < bufferSize; i++ ) {
        struct postponer_buffer_record_type_t* record = &buffer[POS(i)];
        if (record->key == key) {
            if (record->active) {
                *press = record;
            } else {
                *release = record;
                return;
            }
        }
    }
}

void PostponerQuery_InfoByQueueIdx(uint8_t idx, struct postponer_buffer_record_type_t** press, struct postponer_buffer_record_type_t** release)
{
    *press = NULL;
    *release = NULL;
    uint8_t startIdx = getPendingKeypressIdx(idx);
    if(startIdx == 255) {
        return;
    }
    *press = &buffer[POS(startIdx)];
    for ( int i = startIdx; i < bufferSize; i++ ) {
        struct postponer_buffer_record_type_t* record = &buffer[POS(i)];
        if (!record->active && record->key == (*press)->key) {
            *release = record;
            return;
        }
    }
}

