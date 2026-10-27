import unittest

from windowFixes.linePipe import PipeCommandError, encodeArgument, parseReply


class TestReplies(unittest.TestCase):
  def testBareOkHasNoFields(self):
    self.assertEqual(parseReply("ok"), {})

  def testFieldValuesArePercentDecoded(self):
    self.assertEqual(parseReply("ok count=2 persona1Name=Dr%20Brown reason=a%3Db%25c"),
      {"count": "2", "persona1Name": "Dr Brown", "reason": "a=b%c"})

  def testValuesDecodeFromTheAnsiCodePage(self):
    self.assertEqual(parseReply("ok name=Caf%E9"), {"name": "Caf" + chr(0xE9)})

  def testErrorReplyRaisesItsMessage(self):
    with self.assertRaises(PipeCommandError) as raised:
      parseReply("error NPSUserLogin returned -11 (user not found)")
    self.assertEqual(str(raised.exception), "NPSUserLogin returned -11 (user not found)")

  def testFieldWithoutAnEqualsSignIsMalformed(self):
    with self.assertRaises(PipeCommandError) as raised:
      parseReply("ok count")
    self.assertEqual(str(raised.exception), "malformed field 'count' in reply 'ok count'")

  def testReplyThatIsNeitherOkNorErrorIsMalformed(self):
    with self.assertRaises(PipeCommandError) as raised:
      parseReply("okay")
    self.assertEqual(str(raised.exception), "malformed reply 'okay'")


class TestArguments(unittest.TestCase):
  def testSpacesPercentSignsAndEqualsSignsAreEncoded(self):
    self.assertEqual(encodeArgument("Dr Brown 100%=x"), "Dr%20Brown%20100%25%3Dx")

  def testNumbersAreSentAsDecimalText(self):
    self.assertEqual(encodeArgument(4294967295), "4294967295")

  def testTextIsEncodedInTheAnsiCodePage(self):
    self.assertEqual(encodeArgument("Caf" + chr(0xE9)), "Caf%E9")


if __name__ == "__main__":
  unittest.main()
