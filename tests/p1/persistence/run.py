from pathlib import Path
import subprocess,tempfile,os
root=Path(__file__).resolve().parents[3]
s=(root/'AIApps/ChatServer/src/main.cpp').read_text()
body=s[s.index('void executeMysql('):s.index('// 带重试的依赖等待')]
prefix=r'''
#include <functional>
#include <fstream>
#include <chrono>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <nlohmann/json.hpp>
#define LOG_ERROR std::cerr
struct MQManager {enum class Disposition {Ack, Retry, Reject};using AckFn=std::function<void(Disposition)>;};
int writes=0;bool fail_db=false;std::string query;
namespace http {struct MysqlUtil {template<class... Args>int executeUpdate(const std::string& s,Args&&...){++writes;query=s;if(fail_db)throw std::runtime_error("mock DB unavailable");return 1;}};}
'''
header=root/'AIApps/ChatServer/include/AIUtil/ChatMessage.h'
if header.exists():prefix+='#include "AIApps/ChatServer/include/AIUtil/ChatMessage.h"\n'
main=r'''
int main(){int fails=0,acks=0,retries=0,rejects=0;
auto done=[&](auto d){if constexpr(std::is_same_v<decltype(d),bool>){if(d)++acks;else ++retries;}else{if(d==MQManager::Disposition::Ack)++acks;else if(d==MQManager::Disposition::Retry)++retries;else ++rejects;}};
auto check=[&](bool ok,const char* name){std::cout<<(ok?"PASS ":"FAIL ")<<name<<"\n";fails+=!ok;};
executeMysql("INSERT INTO users VALUES ('legacy unsafe SQL')",1,done);
check(writes==0 && acks==0 && rejects==1,"legacy SQL quarantined without execution");
writes=acks=retries=rejects=0;fail_db=true;
std::string envelope=R"({"v":1,"message_id":"0123456789abcdef0123456789abcdef","user_id":1,"username":"u' OR 1=1 --","session_id":"safe-session","question":"quote'\\value","answer":"answer","ts":1234})";
executeMysql(envelope,2,done);check(acks==0&&retries==1,"DB failure does not ACK even when fallback unavailable");
writes=acks=retries=rejects=0;fail_db=false;executeMysql(envelope,3,done);
check(acks==1&&writes==1,"completed exchange persisted atomically before ACK");
check(query.find('?')!=std::string::npos&&query.find("OR 1=1")==std::string::npos,"payload remains bound values rather than SQL text");
writes=acks=retries=rejects=0;
std::string fractional=envelope;fractional.replace(fractional.find("\"v\":1"),5,"\"v\":1.5");
executeMysql(fractional,4,done);check(writes==0&&acks==0&&rejects==1,"fractional schema version rejected before database call");
std::cout<<"failures="<<fails<<"\n";return fails?1:0;}
'''
out=Path(tempfile.mkdtemp(prefix='mycpp-persistence-'));(out/'failed_sql.log').mkdir()
(out/'test.cpp').write_text(prefix+body+main)
subprocess.run(['clang++','-std=c++17','-I'+str(root),'-I'+os.environ.get('MYCPP_TEST_DEPS','/private/tmp/mycpp-p1-deps'),str(out/'test.cpp'),'-o',str(out/'test')],check=True)
r=subprocess.run([str(out/'test')],cwd=out,text=True,capture_output=True)
print(r.stdout);print(r.stderr);print('evidence:',out);(out/'result.log').write_text(r.stdout+r.stderr);raise SystemExit(r.returncode)
