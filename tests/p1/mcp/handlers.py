import sys
sys.dont_write_bytecode = True
from common import ROOT, function, run_fixture

replacements = {}
for name in ('ChatSendHandler', 'ChatCreateAndSendHandler'):
    source = (ROOT / ('AIApps/ChatServer/src/handlers/' + name + '.cpp')).read_text()
    replacements['// @' + name.upper() + '@'] = function(source, 'void ' + name + '::handle(')
run_fixture('handlers_test.cpp', replacements)
