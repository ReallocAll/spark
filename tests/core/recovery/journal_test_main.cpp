#include <iostream>

#include "journal_test_cases.h"

int main()
{
    testFileHeaderMagic();
    testRecordSerialization();
    testModuleDefRoundTrip();
    testSampleRoundTrip();
    testWriterBasic();
    testWriterStopJoins();
    testSessionIsolation();
    testSessionConfigRoundTrip();
    testSessionConfigTrailingBytesRejected();
    testLegacyV2Replay();
    testUnsupportedAndMixedVersionsRejected();
    testV3GlobalWindowsAndClippedStats();
    testRecoveryPlayerReplay();
    testRecoveryPlayerEmptyJournal();
    testCleanEndDetected();
    testNoCleanEndRecovered();
    testLiveOnlyRefused();
    testCleanEndEarlyExit();
    testNonContiguousModuleId();
    testMissingModuleDefReferenced();
    testRollingJournalRecovery();
    testCorruptSnapshotWrongSession();
    testTruncatedSnapshot();
    testAllocationSentinelModule0();
    testMissingSentinelModule0();
    testStopWithoutCleanEndRecoverable();
    testSnapshotOnlyThreadIsNotExported();
    testLegacyV2RebasesFromMinimumRetainedWindow();
    testRecoveryHistoryExecution();
    testRecoveryHistoryCumulativeAllocation();
    testRecoveryRejectsExtremeIntegers();
    std::cout << "All journal tests passed.\n";
    return 0;
}
