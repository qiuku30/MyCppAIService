#!/usr/bin/env python3
from pathlib import Path
import os,sys,subprocess,tempfile,shlex
root=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(root/'tests/p1/mcp'))
from common import function
text=(root/'AIApps/ChatServer/src/AIUtil/AIHelper.cpp').read_text()
s=(Path(__file__).with_name('rag_test.cpp')).read_text()
for marker,signature in [('// @CURL_STREAM@','void AIHelper::executeCurlStream('),('// @CURL_SYNC@','json AIHelper::executeCurl('),('// @WRITE_CALLBACK@','size_t AIHelper::WriteCallback(')]:
 s=s.replace(marker,function(text,signature))
with tempfile.TemporaryDirectory(prefix='mycpp-p2-rag-') as tmp:
 out=Path(tmp);(out/'test.cpp').write_text(s)
 subprocess.run([os.environ.get('CXX','clang++'),'-std=c++17','-pthread','-g','-O1','-Wall','-Wextra',
  '-I'+str(root/'AIApps/ChatServer/include'),'-I'+os.environ.get('MYCPP_TEST_DEPS','/private/tmp/mycpp-p1-deps'),
  *shlex.split(os.environ.get('P2_TEST_CXXFLAGS','')), str(out/'test.cpp'),
  str(root/'AIApps/ChatServer/src/AIUtil/AIStrategy.cpp'),str(root/'AIApps/ChatServer/src/AIUtil/AIFactory.cpp'),'-o',str(out/'test')],check=True)
 result=subprocess.run([str(out/'test')],capture_output=True,text=True)
 print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
 raise SystemExit(result.returncode)
