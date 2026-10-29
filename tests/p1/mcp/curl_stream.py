import sys
sys.dont_write_bytecode = True
from common import ROOT, function, run_fixture

source = (ROOT / 'AIApps/ChatServer/src/AIUtil/AIHelper.cpp').read_text()
strategy = (ROOT / 'AIApps/ChatServer/src/AIUtil/AIStrategy.cpp').read_text()
run_fixture('curl_stream_test.cpp', {
    '// @PRODUCTION_STREAM@': function(source, 'void AIHelper::executeCurlStream('),
    '// @PRODUCTION_CHUNK_PARSER@': function(strategy, 'bool AIStrategy::parseStreamChunk('),
})
