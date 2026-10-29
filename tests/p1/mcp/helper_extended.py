import sys
sys.dont_write_bytecode = True
from common import ROOT, function, run_fixture

source = (ROOT / 'AIApps/ChatServer/src/AIUtil/AIHelper.cpp').read_text()
methods = '\n\n'.join(function(source, signature) for signature in (
    'void AIHelper::setStrategy(', 'void AIHelper::restoreMessage(',
    'void AIHelper::restoreMessage(const std::string& content, long long ms, bool isUser)',
    'void AIHelper::validateCanonicalHistory(',
    'std::vector<AIHelper::HistoryMessage> AIHelper::GetHistorySnapshot(',
    'std::string AIHelper::chat(', 'void AIHelper::chatStream(', 'json AIHelper::request(',
    'std::vector<std::pair<std::string, long long>> AIHelper::GetMessages(',
    'void AIHelper::commitExchange(',
))
run_fixture('helper_extended_test.cpp', {'// @PRODUCTION_METHODS@': methods})
