#include "sx126x_test_helpers.h"
#include <tuple>

namespace esphome::sx126x::testing {

class SX126xBusySetup : public ::testing::TestWithParam<std::tuple<bool, bool>> {};

TEST_P(SX126xBusySetup, StopsAtEveryConfigurationCommand) {
  const auto [lora, rx] = GetParam();
  RadioHardware reference;
  reference.configure(lora, rx);
  ASSERT_FALSE(reference.radio.is_failed());
  ASSERT_FALSE(reference.radio.status_has_warning());
  const size_t count = reference.transactions.size();
  ASSERT_GT(count, 10u);
  for (size_t fault = 1; fault <= count; fault++) {
    SCOPED_TRACE(fault);
    RadioHardware hardware;
    hardware.fail_at = fault;
    hardware.configure(lora, rx);
    hardware.expect_aborted(fault);
    EXPECT_TRUE(hardware.radio.is_failed());
    hardware.radio.dump_config();  // Also safe if the version read never happened.
    hardware.clear_trace();
    hardware.radio.configure();
    hardware.radio.set_mode_rx();
    hardware.radio.set_mode_tx();
    hardware.radio.set_mode_sleep();
    hardware.radio.set_mode_standby(STDBY_XOSC);
    hardware.radio.run_image_cal();
    hardware.radio.loop();
    EXPECT_EQ(hardware.radio.transmit_packet(hardware.packet), SX126xError::SX126X_ERROR_NOT_READY);
    EXPECT_TRUE(hardware.transactions.empty());
  }
}

INSTANTIATE_TEST_SUITE_P(LoRaAndFSKWithRxOrSleep, SX126xBusySetup,
                         ::testing::Combine(::testing::Bool(), ::testing::Bool()));

class SX126xBusyTransmit : public ::testing::TestWithParam<std::tuple<bool, bool, bool>> {};

TEST_P(SX126xBusyTransmit, StopsAtEverySendAndCleanupCommandAndCanRetry) {
  const auto [lora, rx, fixed] = GetParam();
  RadioHardware reference;
  reference.configure(lora, rx, fixed);
  reference.clear_trace();
  ASSERT_EQ(reference.radio.transmit_packet(reference.packet), SX126xError::NONE);
  const size_t count = reference.transactions.size();
  for (size_t fault = 1; fault <= count; fault++) {
    SCOPED_TRACE(fault);
    RadioHardware hardware;
    hardware.configure(lora, rx, fixed);
    hardware.clear_trace();
    hardware.fail_at = fault;
    EXPECT_EQ(hardware.radio.transmit_packet(hardware.packet.data(), hardware.packet.size()),
              SX126xError::SX126X_ERROR_BUSY_TIMEOUT);
    hardware.expect_aborted(fault);
    EXPECT_TRUE(hardware.radio.status_has_warning());
    EXPECT_FALSE(hardware.radio.is_failed());
    if (hardware.sends == 0)
      EXPECT_EQ(hardware.dio.reads, 0u);  // Never enter the 4-second TX wait after a failed start.
    hardware.clear_trace();
    EXPECT_EQ(hardware.radio.transmit_packet(hardware.packet), SX126xError::NONE);
    EXPECT_FALSE(hardware.radio.status_has_warning());
    EXPECT_EQ(hardware.receiving, rx);
  }
}

INSTANTIATE_TEST_SUITE_P(LoRaAndFSKWithRxOrSleepAndFixedOrVariablePayload, SX126xBusyTransmit,
                         ::testing::Combine(::testing::Bool(), ::testing::Bool(), ::testing::Bool()));

class SX126xBusyReceive : public ::testing::TestWithParam<bool> {};

TEST_P(SX126xBusyReceive, DropsFailedReadsAndRecoversOnNextPacket) {
  const bool lora = GetParam();
  for (size_t fault = 1; fault <= 5; fault++) {
    SCOPED_TRACE(fault);
    RadioHardware hardware;
    PacketListener listener;
    hardware.configure(lora, true);
    hardware.radio.register_listener(&listener);
    hardware.clear_trace();
    hardware.irq = IRQ_RX_DONE;
    hardware.fail_at = fault;
    hardware.radio.loop();
    hardware.expect_aborted(fault);
    EXPECT_TRUE(listener.packets.empty());
    EXPECT_TRUE(hardware.radio.status_has_warning());
    EXPECT_FALSE(hardware.radio.is_failed());
    hardware.clear_trace();
    hardware.irq = IRQ_RX_DONE;
    hardware.radio.loop();
    ASSERT_EQ(listener.packets.size(), 1u);
    EXPECT_EQ(listener.packets[0], hardware.packet);
    EXPECT_FLOAT_EQ(listener.rssi, lora ? -40 : -42);
    EXPECT_FLOAT_EQ(listener.snr, lora ? 10 : 0);
    EXPECT_FALSE(hardware.radio.status_has_warning());
  }
}

INSTANTIATE_TEST_SUITE_P(LoRaAndFSK, SX126xBusyReceive, ::testing::Bool());

TEST(SX126xBusy, KeepsPollingAnUnclearedInterruptAfterBusyTimeout) {
  RadioHardware hardware;
  PacketListener listener;
  hardware.configure(true, true);
  hardware.radio.register_listener(&listener);
  hardware.dio.internal = true;  // No new ISR edge while the pending IRQ stays high.
  hardware.clear_trace();
  hardware.fail_at = 1;
  hardware.irq = IRQ_RX_DONE;
  hardware.radio.call();
  EXPECT_TRUE(listener.packets.empty());
  hardware.clear_trace();
  hardware.radio.call();
  EXPECT_EQ(listener.packets.size(), 1u);
}

TEST(SX126xBusy, AssertsChipSelectBeforeWaitingForWakeup) {
  RadioHardware hardware;
  hardware.configure(true, true);
  hardware.clear_trace();
  hardware.busy.level = true;  // The CS callback releases BUSY only after selecting the chip.
  EXPECT_EQ(hardware.radio.transmit_packet(hardware.packet), SX126xError::NONE);
  EXPECT_TRUE(hardware.cs.level);
  EXPECT_EQ(hardware.ends, hardware.transactions.size());
}

TEST(SX126xBusy, RejectsSendBeforeSetupWithoutTouchingPins) {
  RadioHardware hardware;
  EXPECT_EQ(hardware.radio.transmit_packet(hardware.packet), SX126xError::SX126X_ERROR_NOT_READY);
  EXPECT_TRUE(hardware.transactions.empty());
  EXPECT_EQ(hardware.busy.reads, 0u);
  EXPECT_EQ(hardware.dio.reads, 0u);
}

TEST(SX126xBusy, FailedReconfigurationRequiresSuccessfulConfigureBeforeOperations) {
  RadioHardware reference;
  reference.configure(true, true);
  const size_t count = reference.transactions.size();
  for (size_t fault = 1; fault <= count; fault++) {
    SCOPED_TRACE(fault);
    RadioHardware hw;
    PacketListener listener;
    hw.configure(true, true);
    hw.radio.register_listener(&listener);
    hw.clear_trace();
    hw.fail_at = fault;
    hw.radio.configure();
    EXPECT_FALSE(hw.radio.is_failed());
    hw.clear_trace();
    EXPECT_EQ(hw.radio.transmit_packet(hw.packet), SX126xError::SX126X_ERROR_NOT_READY);
    hw.irq = IRQ_RX_DONE;
    hw.radio.loop();
    hw.radio.set_mode_rx();
    hw.radio.set_mode_tx();
    hw.radio.set_mode_standby(STDBY_XOSC);
    hw.radio.set_mode_sleep();
    hw.radio.run_image_cal();
    EXPECT_TRUE(hw.transactions.empty());
    EXPECT_TRUE(listener.packets.empty());
    hw.radio.configure();
    EXPECT_FALSE(hw.radio.status_has_warning());
    EXPECT_EQ(hw.radio.transmit_packet(hw.packet), SX126xError::NONE);
  }
}

TEST(SX126xBusy, RetryClearsStaleTxDoneBeforeWaitingForDelayedCompletion) {
  RadioHardware reference;
  reference.configure(true, true);
  reference.clear_trace();
  reference.radio.transmit_packet(reference.packet);
  size_t cleanup = 0;
  bool sent = false;
  for (size_t i = 0; i < reference.transactions.size(); i++) {
    if (reference.transactions[i][0] == RADIO_SET_TX)
      sent = true;
    if (sent && reference.transactions[i][0] == RADIO_CLR_IRQSTATUS) {
      cleanup = i + 1;
      break;
    }
  }
  ASSERT_NE(cleanup, 0u);
  RadioHardware hw;
  hw.configure(true, true);
  hw.clear_trace();
  hw.fail_at = cleanup;
  ASSERT_EQ(hw.radio.transmit_packet(hw.packet), SX126xError::SX126X_ERROR_BUSY_TIMEOUT);
  ASSERT_NE(hw.irq & IRQ_TX_DONE, 0);
  hw.clear_trace();
  hw.auto_complete = false;
  unsigned polls = 0;
  hw.dio.on_read = [&]() {
    if (++polls == 5)
      hw.irq |= IRQ_TX_DONE;
    return hw.irq != 0;
  };
  EXPECT_EQ(hw.radio.transmit_packet(hw.packet), SX126xError::NONE);
  EXPECT_EQ(polls, 5u);
  EXPECT_TRUE(hw.receiving);
}

}  // namespace esphome::sx126x::testing
