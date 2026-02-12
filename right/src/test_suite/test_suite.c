#include "test_suite.h"
#include "test_hooks.h"
#include "test_actions.h"
#include "test_input_machine.h"
#include "test_output_machine.h"
#include "tests/tests.h"
#include "logger.h"
#include "timer.h"
#include "usb_interfaces/usb_interface_basic_keyboard.h"
#include "keymap.h"
#include "config_manager.h"
#include "macros/vars.h"
#include "mouse_controller.h"

#if defined(__ZEPHYR__) && DEVICE_IS_KEYBOARD
#include "keyboard/battery_unloaded_calculator.h"
#include "keyboard/battery_percent_calculator.h"
#endif


#define INTER_TEST_DELAY___MS 100

// Test hooks state
bool TestHooks_Active = false;
bool TestSuite_Verbose = false;

// Test tracking
static uint16_t currentModuleIndex = 0;
static uint16_t currentTestIndex = 0;
static uint16_t totalTestCount = 0;
static uint16_t passedCount = 0;
static uint16_t failedCount = 0;

// Single test mode
static bool singleTestMode = false;

// Rerun state for failed tests
static bool isRerunning = false;
static uint16_t rerunModuleIndex = 0;
static uint16_t rerunTestIndex = 0;
static uint8_t rerunEnvPass = 0;

// Inter-test delay state
static bool inInterTestDelay = false;
static uint32_t interTestDelayStart = 0;

// Environment pass tracking
// Pass 0 = no environment, Pass 1+ = specific environments
static uint8_t currentEnvPass = 0;
#define ENV_PASS_NONE 0
#define ENV_PASS_POSTPONING 1
#define ENV_PASS_COUNT 2  // Total number of passes (none + postponing)

// Test phases within an environment pass
typedef enum {
    TestPhase_Prologue,
    TestPhase_Main,
    TestPhase_Epilogue,
} test_phase_t;

static test_phase_t currentPhase = TestPhase_Main;

// Postponing environment prologue: set up postpone key and press it
static const test_action_t envPostponingPrologue[] = {
    TEST_SET_MACRO("y", "postponeKeys delayUntilRelease\n"),
    TEST_PRESS______("y"),
    TEST_DELAY__(20),
    TEST_END()
};

// Postponing environment epilogue: verify postponing works then release
static const test_action_t envPostponingEpilogue[] = {
    TEST_SET_ACTION("m", "m"),
    TEST_PRESS______("m"),
    TEST_DELAY__(20),
    TEST_CHECK_NOW(""),           // Verify evaluation is postponed
    TEST_EXPECT__________("m"),   // Will appear after postpone key released
    TEST_RELEASE__U("m"),
    TEST_DELAY__(20),
    TEST_RELEASE__U("y"),
    TEST_DELAY__(50),
    TEST_EXPECT__________(""),
    TEST_END()
};

static const test_t* getCurrentTest(void) {
    return &AllTestModules[currentModuleIndex]->tests[currentTestIndex];
}

// Check if test should run in current environment pass
static bool testMatchesEnvPass(const test_t *test, uint8_t envPass) {
    if (envPass == ENV_PASS_NONE) {
        return true;  // All tests run in pass 0
    }
    if (envPass == ENV_PASS_POSTPONING) {
        return (test->envFlags & TEST_ENV_POSTPONING) != 0;
    }
    return false;
}

// Find next test that matches the current environment pass
static bool advanceToNextTest(void) {
    while (true) {
        currentTestIndex++;
        if (currentTestIndex >= AllTestModules[currentModuleIndex]->testCount) {
            currentModuleIndex++;
            currentTestIndex = 0;
        }
        if (currentModuleIndex >= AllTestModulesCount) {
            return false;
        }
        // Check if test matches current environment pass
        const test_t *test = getCurrentTest();
        if (testMatchesEnvPass(test, currentEnvPass)) {
            return true;
        }
    }
}

// Find first test that matches the current environment pass
static bool findFirstMatchingTest(void) {
    currentModuleIndex = 0;
    currentTestIndex = 0;
    if (AllTestModulesCount == 0) {
        return false;
    }
    const test_t *test = getCurrentTest();
    if (testMatchesEnvPass(test, currentEnvPass)) {
        return true;
    }
    return advanceToNextTest();
}

// Get current actions based on phase
static const test_action_t* getCurrentActions(const test_t *test) {
    if (currentEnvPass == ENV_PASS_NONE) {
        return test->actions;
    }
    switch (currentPhase) {
        case TestPhase_Prologue:
            if (currentEnvPass == ENV_PASS_POSTPONING) {
                return envPostponingPrologue;
            }
            break;
        case TestPhase_Main:
            return test->actions;
        case TestPhase_Epilogue:
            if (currentEnvPass == ENV_PASS_POSTPONING) {
                return envPostponingEpilogue;
            }
            break;
    }
    return test->actions;
}

// Wrapper test for current phase
static test_t phaseTest;

static void startPhase(const test_t *test, const test_module_t *module) {
    phaseTest.name = test->name;
    phaseTest.actions = getCurrentActions(test);
    phaseTest.envFlags = test->envFlags;

    InputMachine_Start(&phaseTest);
    OutputMachine_Start(&phaseTest);
    OutputMachine_OnReportChange(ActiveUsbBasicKeyboardReport);
}

static void startTest(const test_t *test, const test_module_t *module) {
    ConfigManager_ResetConfiguration(false);

    // Determine starting phase based on environment
    if (currentEnvPass == ENV_PASS_NONE) {
        currentPhase = TestPhase_Main;
    } else {
        currentPhase = TestPhase_Prologue;
    }

    if (TestSuite_Verbose) {
        LogU("[TEST] ----------------------\n");
        if (currentEnvPass == ENV_PASS_NONE) {
            LogU("[TEST] Running: %s/%s\n", module->name, test->name);
        } else {
            LogU("[TEST] Running: %s/%s [env:postponing]\n", module->name, test->name);
        }
    }

    startPhase(test, module);
}

// Advance to next phase, returns true if there is a next phase
static bool advanceToNextPhase(void) {
    if (currentEnvPass == ENV_PASS_NONE) {
        return false;  // No phases in pass 0
    }
    switch (currentPhase) {
        case TestPhase_Prologue:
            currentPhase = TestPhase_Main;
            return true;
        case TestPhase_Main:
            currentPhase = TestPhase_Epilogue;
            return true;
        case TestPhase_Epilogue:
            return false;
    }
    return false;
}

void TestHooks_CaptureReport(const usb_basic_keyboard_report_t *report) {
    if (!TestHooks_Active) {
        return;
    }
    OutputMachine_OnReportChange(report);
}

// Advance to next environment pass, returns true if there is a next pass
static bool advanceToNextEnvPass(void) {
    currentEnvPass++;
    if (currentEnvPass >= ENV_PASS_COUNT) {
        return false;
    }
    return findFirstMatchingTest();
}

void TestHooks_Tick(void) {
    if (!TestHooks_Active) {
        return;
    }

    // Handle inter-test delay
    if (inInterTestDelay) {
        if (Timer_GetElapsedTime(&interTestDelayStart) >= INTER_TEST_DELAY___MS) {
            inInterTestDelay = false;
            const test_t *nextTest = getCurrentTest();
            const test_module_t *module = AllTestModules[currentModuleIndex];
            startTest(nextTest, module);
        }
        return;
    }

    InputMachine_Tick();

    // Check for completion or failure
    bool inputDone = InputMachine_IsDone();
    bool outputDone = OutputMachine_IsDone();
    bool failed = InputMachine_Failed || OutputMachine_Failed;
    bool timedOut = InputMachine_TimedOut && !outputDone;

    if (inputDone && (outputDone || timedOut || failed)) {
        const test_t *test = getCurrentTest();
        const test_module_t *module = AllTestModules[currentModuleIndex];

        if (failed || timedOut) {
            if (isRerunning || singleTestMode) {
                // Already rerunning with verbose (or single test mode), log final result
                const char *envSuffix = (currentEnvPass == ENV_PASS_POSTPONING) ? " [env:postponing]" : "";
                if (failed) {
                    LogU("[TEST] Finished: %s/%s%s - FAIL\n", module->name, test->name, envSuffix);
                } else {
                    LogU("[TEST] Finished: %s/%s%s - TIMEOUT\n", module->name, test->name, envSuffix);
                }
                LogU("[TEST] ----------------------\n");
                failedCount++;
                isRerunning = false;
                TestSuite_Verbose = false;  // Reset to non-verbose for remaining tests

                if (singleTestMode) {
                    goto finish;
                }

                // Continue from where we left off
                currentModuleIndex = rerunModuleIndex;
                currentTestIndex = rerunTestIndex;
                currentEnvPass = rerunEnvPass;
                if (advanceToNextTest()) {
                    inInterTestDelay = true;
                    interTestDelayStart = Timer_GetCurrentTime();
                } else if (advanceToNextEnvPass()) {
                    inInterTestDelay = true;
                    interTestDelayStart = Timer_GetCurrentTime();
                } else {
                    goto finish;
                }
            } else {
                // First failure - log and save position for rerun with verbose
                const char *envSuffix = (currentEnvPass == ENV_PASS_POSTPONING) ? " [env:postponing]" : "";
                if (failed) {
                    LogU("[TEST] Finished: %s/%s%s - FAIL (rerunning verbose)\n", module->name, test->name, envSuffix);
                } else {
                    LogU("[TEST] Finished: %s/%s%s - TIMEOUT (rerunning verbose)\n", module->name, test->name, envSuffix);
                }
                rerunModuleIndex = currentModuleIndex;
                rerunTestIndex = currentTestIndex;
                rerunEnvPass = currentEnvPass;
                isRerunning = true;
                TestSuite_Verbose = true;

                inInterTestDelay = true;
                interTestDelayStart = Timer_GetCurrentTime();
            }
        } else {
            // Phase completed successfully - check if there are more phases
            if (advanceToNextPhase()) {
                // Start next phase of same test
                startPhase(test, module);
                return;
            }

            // All phases complete - test passed
            const char *envSuffix = (currentEnvPass == ENV_PASS_POSTPONING) ? " [env:postponing]" : "";
            LogU("[TEST] Finished: %s/%s%s - PASS\n", module->name, test->name, envSuffix);
            passedCount++;
            if (isRerunning) {
                isRerunning = false;
                TestSuite_Verbose = false;  // Reset to non-verbose for remaining tests
            }

            if (singleTestMode) {
                goto finish;
            }

            // Move to next test
            if (advanceToNextTest()) {
                inInterTestDelay = true;
                interTestDelayStart = Timer_GetCurrentTime();
            } else if (advanceToNextEnvPass()) {
                inInterTestDelay = true;
                interTestDelayStart = Timer_GetCurrentTime();
            } else {
                goto finish;
            }
        }
    }
    return;

finish:
    LogU("[TEST] ----------------------\n");
    LogU("[TEST] Complete: %d passed, %d failed\n", passedCount, failedCount);
    TestHooks_Active = false;
    ConfigManager_ResetConfiguration(false);
}

void TestSuite_Init(void) {
    TestHooks_Active = false;
}

uint8_t TestSuite_RunAll(void) {
    currentModuleIndex = 0;
    currentTestIndex = 0;
    passedCount = 0;
    failedCount = 0;
    inInterTestDelay = false;
    isRerunning = false;
    singleTestMode = false;
    TestSuite_Verbose = false;
    currentEnvPass = ENV_PASS_NONE;
    currentPhase = TestPhase_Main;

    // Count total tests (base + environment tests)
    totalTestCount = 0;
    uint16_t envTestCount = 0;
    for (uint16_t i = 0; i < AllTestModulesCount; i++) {
        totalTestCount += AllTestModules[i]->testCount;
        for (uint16_t j = 0; j < AllTestModules[i]->testCount; j++) {
            if (AllTestModules[i]->tests[j].envFlags & TEST_ENV_POSTPONING) {
                envTestCount++;
            }
        }
    }
    totalTestCount += envTestCount;  // Add environment pass tests

    LogU("[TEST] Running custom unit tests...\n");

    MacroVariables_RunTests();
#if defined(__ZEPHYR__) && DEVICE_IS_KEYBOARD
    BatteryCalculator_RunTests();
    BatteryCalculator_RunPercentTests();
#endif

    LogU("[TEST] Starting test suite (%d tests in %d modules, +%d env tests)\n",
         totalTestCount - envTestCount, AllTestModulesCount, envTestCount);

    if (totalTestCount == 0) {
        return 0;
    }

    // Start first test
    const test_t *firstTest = getCurrentTest();
    const test_module_t *module = AllTestModules[currentModuleIndex];
    startTest(firstTest, module);
    TestHooks_Active = true;

    return totalTestCount;
}

static bool streq(const char *a, const char *aEnd, const char *b) {
    while (a < aEnd && *b) {
        if (*a++ != *b++) return false;
    }
    return a == aEnd && *b == '\0';
}

uint8_t TestSuite_RunSingle(const char *moduleStart, const char *moduleEnd, const char *testStart, const char *testEnd) {
    // Find the module and test
    for (uint16_t mi = 0; mi < AllTestModulesCount; mi++) {
        const test_module_t *module = AllTestModules[mi];
        if (!streq(moduleStart, moduleEnd, module->name)) continue;

        for (uint16_t ti = 0; ti < module->testCount; ti++) {
            const test_t *test = &module->tests[ti];
            if (!streq(testStart, testEnd, test->name)) continue;

            // Found it - run with verbose logging
            currentModuleIndex = mi;
            currentTestIndex = ti;
            passedCount = 0;
            failedCount = 0;
            inInterTestDelay = false;
            isRerunning = false;
            singleTestMode = true;
            TestSuite_Verbose = true;  // Always verbose for single test
            currentEnvPass = ENV_PASS_NONE;
            currentPhase = TestPhase_Main;

            LogU("[TEST] Running single test: %s/%s\n", module->name, test->name);
            startTest(test, module);
            TestHooks_Active = true;

            return 0;
        }
    }

    LogU("[TEST] Test not found: %.*s/%.*s\n",
        (int)(moduleEnd - moduleStart), moduleStart,
        (int)(testEnd - testStart), testStart);
    return 255;
}
