// Simple harness to compare Qt regex behavior against MFC/PCRE expectations.
// This is intentionally lightweight and prints raw results for manual review.

#include <QCoreApplication>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <vector>

struct RegexTest
{
  const char* id;
  const char* pattern;
  const char* subject;
  bool ignoreCase;
  bool matchEmpty;
};

static void runTest(const RegexTest& test, QTextStream& out)
{
  QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
  if (test.ignoreCase)
    options |= QRegularExpression::CaseInsensitiveOption;

  QRegularExpression regex(QString::fromUtf8(test.pattern), options);
  out << "[" << test.id << "] pattern=" << test.pattern << " subject=" << test.subject << "\n";

  if (!regex.isValid())
    {
    out << "  invalid: " << regex.errorString() << " (offset " << regex.patternErrorOffset() << ")\n";
    return;
    }

  QRegularExpressionMatch match = regex.match(QString::fromUtf8(test.subject));
  if (!match.hasMatch())
    {
    out << "  no match\n";
    return;
    }

  if (!test.matchEmpty && match.capturedLength(0) == 0)
    {
    out << "  matched empty (treated as no-match when matchEmpty=false)\n";
    return;
    }

  out << "  match: '" << match.captured(0) << "'\n";
  const int lastIndex = match.lastCapturedIndex();
  for (int i = 1; i <= lastIndex; ++i)
    out << "  capture[" << i << "]='" << match.captured(i) << "'\n";

  const QStringList names = regex.namedCaptureGroups();
  for (int i = 1; i < names.size(); ++i)
    {
    const QString name = names.at(i);
    if (name.isEmpty())
      continue;
    out << "  named['" << name << "']='" << match.captured(i) << "'\n";
    }
}

int main(int argc, char** argv)
{
  QCoreApplication app(argc, argv);
  QTextStream out(stdout);

  const std::vector<RegexTest> tests = {
      {"1", "(foo)(bar)", "foobar", false, true},
      {"2", "^", "abc", false, true},
      {"3", "^", "abc", false, false},
      {"4", "hello", "HeLLo", true, true},
      {"5", "(?<word>foo)bar", "foobar", false, true},
      {"6", "(?<x>a)|(?<x>b)", "b", false, true},
      {"9", "(a(?R)?b)", "aab", false, true},
      {"10", "a(?C)b", "ab", false, true},
      {"11", "\\C\\C", "A", false, true},
      {"12", "a\\Rb", "a\r\nb", false, true},
      {"13", "(?>ab|a)b", "ab", false, true},
      {"14", "a(*SKIP)b", "ab", false, true},
      {"15", "(abc", "abc", false, true},
  };

  for (const RegexTest& test : tests)
    runTest(test, out);

  return 0;
}

