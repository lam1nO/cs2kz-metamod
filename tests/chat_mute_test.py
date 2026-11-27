"""The KZ-specific path must check the mute before delivering to any audience."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ChatMuteTest(unittest.TestCase):
    def test_mute_precedes_kz_chat_console_and_race_relay(self):
        source = (ROOT / "src/kz/misc/kz_misc.cpp").read_text()
        dispatch = source.split("void KZ::misc::ProcessConCommand", 1)[1]
        dispatch = dispatch.split("void KZ::misc::OnRoundStart", 1)[0]
        gate = "if (ChatMute::Blocks(player->GetSteamId64()))\n\t\t{\n\t\t\treturn;\n\t\t}"
        self.assertIn(gate, dispatch)
        for output in ("utils::SayChat", "utils::PrintConsoleAll", "racingService->SendChatMessage"):
            self.assertLess(dispatch.index(gate), dispatch.index(output))

    def test_engine_chat_filter_uses_same_switch_as_custom_relay(self):
        quiet = (ROOT / "src/kz/quiet/kz_quiet.cpp").read_text()
        hooks = (ROOT / "src/utils/hooks.cpp").read_text()
        say_filters = quiet.split("case UM_SayText:", 1)[1].split("default:", 1)[0]
        self.assertEqual(say_filters.count("if (!KZ::misc::ShouldOverridePlayerChat())"), 2)
        self.assertIn("if (KZ::misc::ShouldOverridePlayerChat())", hooks)


if __name__ == "__main__":
    unittest.main()
