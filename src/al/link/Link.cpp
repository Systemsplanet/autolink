// Wire-protocol coordinator. State lives in
// LinkArq / LinkReorder / LinkSweep; Link
// composes them and owns I/O.
//
// The implementation is split across multiple
// translation units so each `Link::` method
// cluster can be edited without scrolling through
// 1400 lines. Each TU carries its own static
// `TAG`, heartbeat / fast-idle constants, and the
// `MAX_CHUNK` static_assert:
//
//   LinkCore.cpp    ctor, begin, kickoff,
//                   changeState, resetSeq, getters,
//                   getStats / resetStats / resetErrors /
//                   resetDiag, lockOk
//   LinkTx.cpp      sendFrame, buildAndTxCobs,
//                   sendCobsFrame*, resend, sendCtrl
//                   (ACK/NAK), buildAndSendMsg_unlocked
//   LinkRx.cpp      onRx, processCtrlFrame,
//                   ctrlFrameReady, onPayload,
//                   onAck / onNak / onFrameError,
//                   findMsgHeaderResync, recvMsg
//   LinkSweep.cpp   handleSwp, applyMaster/PongSwpAction,
//                   handleLck, bestSpd, okTickMs,
//                   phase1ArmMs, onTimerSwp
//   LinkTimers.cpp  onBreak, onTimer, onTimerOk / Swp / Lck
//   LinkApi.cpp     write/read/peek/available,
//                   flush / flushRx / dropLink,
//                   err / clearErr, sendMsg,
//                   pendingAcks / isAcked,
//                   test_sendMsg*
//
// The class members stay in Link.h; the helper
// classes (LinkArq, LinkReorder, LinkSweep) own
// their own state per AGENTS rule 14.
//
// This file exists so the existing build paths
// (`$(LINK_SRC)` / `$(AUTOLINK_SRC)` in the test
// Makefiles) still resolve a `Link.cpp` symbol.
// The actual Link::method bodies live in the
// split TUs above.
#include "al/link/Link.h"
