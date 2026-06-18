The Chinon protocol sends 0x00 as a valid param byte ({0x15, 0x00, 0xEA} = SPINDLE_MOTOR_OFF). This works today only because 0x00 is never a
  valid opcode — the check is only in RXD_OPCODE. But this is an invisible contract: nothing in the type system prevents a future PIO timeout
  from returning 0 and being misinterpreted as a legitimate byte, or vice versa.

  The fix is a two-return interface:

  // What it should be
  typedef enum { COMMO_HAL_RX_OK, COMMO_HAL_RX_TIMEOUT } commo_hal_rx_status_t;
  commo_hal_rx_status_t commo_hal_rxd(uint8_t *out);

  commo.c then becomes explicit: if (status == COMMO_HAL_RX_TIMEOUT) rather than if (b == 0).

  Problem 2 — release() is untestable

  The stub has zero observable behavior for release(). A bug where commo_hal_release() is dropped from TXD_DATA or TXD_CHECKSUM is completely
  invisible to the test suite — none of the TX tests assert it was called. A stub fix:

  // Add to commo_hal_stub.h
  int commo_hal_stub_release_count(void);

  Tests can then assert LONGS_EQUAL(1, commo_hal_stub_release_count()) after any TX sequence.

  Problem 3 — silent queue overflow in the stub

  // commo_hal_stub.c
  void commo_hal_stub_push(uint8_t b)
  {
      if (s_rx_tail < (int)sizeof(s_rx_queue))   // silently drops when full
          s_rx_queue[s_rx_tail++] = b;
  }

  A test that accidentally pushes 65 bytes gets the first 64 and gives subtly wrong results rather than a test failure. The stub should track an
  overflow flag that the test teardown can assert against.

  Problem 4 — data_is_low leaks protocol timing into tests

  Look at drive_packet():

  commo_hal_stub_set_data_low(1); COMMO_INTERFACE();  // IDLE → RXD_OPCODE
  for (int i = 0; i < len; i++) {
      commo_hal_stub_set_data_low(1); COMMO_INTERFACE();
  }

  Every test that can't use drive_packet() (like AbortedCommand) has to manually interleave set_data_low and COMMO_INTERFACE() calls. The test is
  modeling hardware timing, not the protocol. A better stub would auto-assert data_is_low when bytes are in the RX queue and clear it after
  rxd() drains the byte — matching what the PIO actually does. The data_is_low control function would still exist for adversarial tests (e.g.,
  TxNotInterruptedByRxStrobe), but the normal path wouldn't need it.

  ★ Insight ─────────────────────────────────────
  The root cause of all four problems is the same: the HAL was designed bottom-up, matching the exact shape of the 8051 hardware operations,
  rather than top-down, asking "what does the state machine need to express?" A top-down design notices immediately that rxd() needs an error
  channel, that release() needs to be observable in tests, and that data_is_low should be a hardware detail the stub hides rather than a knob
  tests have to turn manually.
  ─────────────────────────────────────────────────

  ---
  Rewriting the 8051 firmware from scratch — a plan
  
  The upstream commo.c / cmd_hndl.c / dispatcher.c are 8051 code ported to C. They carry three categories of debt: pointer truncation bugs
  (already fixed), a state machine that predates dual-core, and naming conventions (COMMO_INTERFACE, GET_BUFFER, NEW_CMD_RECEIVED) that are
  all-caps macros masking functions.

  Here's how I'd approach a clean rewrite:

  Phase 1 — Nail down the protocol contract in tests first (no new code yet)

  Before touching a line of production code, write the tests that define correct behavior — using the existing stub infrastructure. The
  CommoProtocol, CommoFuzz, and CommoPowerOn groups already do this. Verify they all pass. These tests become the acceptance criteria for the
  rewrite.

  Phase 2 — Redesign the HAL interface (the fixes above)

  Fix the three stub problems first. Reason: the rewrite will be tested against the new stub. You want the test infrastructure clean before the
  production code changes, not the other way around.

  Phase 3 — Rewrite commo.c as a pure state machine with no global state

  The current commo.c has static commo_ctx_t s_commo as a module-level singleton. A clean rewrite passes context explicitly:

  // commo.h — what you'd design on day one
  typedef struct commo_ctx commo_ctx_t;       // opaque

  void     commo_init(commo_ctx_t *ctx);
  void     commo_tick(commo_ctx_t *ctx);      // renamed from COMMO_INTERFACE
  bool     commo_cmd_pending(commo_ctx_t *ctx, commo_cmd_t *out);   // replaces NEW_CMD_RECEIVED + GET_BUFFER
  bool     commo_send(commo_ctx_t *ctx, const uint8_t *data, uint8_t len, bool with_checksum);
  bool     commo_send_ready(commo_ctx_t *ctx);
  void     commo_cmd_consumed(commo_ctx_t *ctx);  // replaces FREE_CMD_BUFFER

  Two wins: (a) the singleton is gone so tests can run multiple independent instances in parallel, and (b) commo_cmd_t is a struct that carries
  both the opcode and params together, eliminating the GET_BUFFER(idx) index-based access that's easy to misuse.

  Phase 4 — Rewrite cmd_hndl.c as a dispatch table

  The current cmd_hndl.c is a switch/case on opcode with side effects sprayed through it. A clean version is a table:

  typedef void (*opcode_handler_t)(const commo_cmd_t *cmd, drive_state_t *state);

  static const opcode_handler_t s_handlers[256] = {
      [STOP_OPC]         = handle_stop,
      [PLAY_TRACK_OPC]   = handle_play_track,
      [SEEK_OPC]         = handle_seek,
      // ...
  };

  This is directly testable: OpcResponses becomes a loop over s_handlers[opcode](cmd, &state) with table-driven expected outputs. Adding a new
  opcode is adding one row to the table and one function, not finding the right place in a switch statement.

  Phase 5 — Collapse dispatcher.c into commo_bridge.c

  dispatcher.c is 40 lines that routes packets between commo.c and cmd_hndl.c. commo_bridge.c already does the real work. On a Pico, there's no
  bus to dispatch — commo_bridge_poll() is the main loop. Eliminating dispatcher.c as a separate file removes one layer of indirection with no
  architectural benefit.

  Phase 6 — Run the existing test suite against the rewrite

  The 605-test suite is the safety net. If CommoProtocol, CommoFuzz, CommoPowerOn, and OpcResponses all pass unchanged after the rewrite, the
  protocol behavior is preserved.

  ★ Insight ─────────────────────────────────────
  The key discipline in a firmware rewrite is: tests before code, not tests after code. The existing test suite is already good enough to be the
  spec. Write no new production code until you have a red test, then make it green. The 8051-derived naming (COMMO_INTERFACE, GET_BUFFER) is a
  symptom — when you rename to commo_tick / commo_cmd_pending, the tests immediately read as protocol-level assertions rather than hardware
  register polls.
  ─────────────────────────────────────────────────

