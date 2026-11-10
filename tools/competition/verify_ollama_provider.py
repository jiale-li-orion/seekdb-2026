#!/usr/bin/env python3
"""Compile the production Ollama non-streaming request builder against narrow C++ stubs."""
from pathlib import Path
import subprocess
import tempfile

src = (Path(__file__).resolve().parents[2] / 'src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp').read_text()
start = src.index('int ObOllamaUtils::ObOllamaComplete::get_body(')
end = src.index('int ObOllamaUtils::ObOllamaComplete::set_config_json_format(', start)
header = r'''
#include <string>
#include <unordered_map>
#include <iostream>
#include <cstdlib>
#include <utility>
#define OB_SUCCESS 0
#define OB_INVALID_ARGUMENT 1
#define OB_FAIL(x) ((ret=(x)) != OB_SUCCESS)
#define OB_SUCC(x) ((x) == OB_SUCCESS)
#define OB_NOT_NULL(x) ((x) != nullptr)
struct ObIAllocator {};
class ObString {public:explicit ObString(std::string v):v_(std::move(v)){}bool empty()const{return v_.empty();}const std::string& text()const{return v_;}private:std::string v_;};
enum class ObJsonNodeType {J_STRING,J_BOOLEAN,J_OBJECT};
struct ObJsonNode{virtual ObJsonNodeType json_type()const=0;virtual bool get_boolean()const{return false;}virtual ~ObJsonNode()=default;};
struct ObJsonString:ObJsonNode{std::string value;explicit ObJsonString(std::string v):value(std::move(v)){}ObJsonNodeType json_type()const override{return ObJsonNodeType::J_STRING;}};
struct ObJsonBoolean:ObJsonNode{bool value;explicit ObJsonBoolean(bool x):value(x){}ObJsonNodeType json_type()const override{return ObJsonNodeType::J_BOOLEAN;}bool get_boolean()const override{return value;}};
struct ObJsonObject:ObJsonNode{std::unordered_map<std::string,ObJsonNode*> entries;ObJsonNodeType json_type()const override{return ObJsonNodeType::J_OBJECT;}ObJsonNode*get_value(const char*key)const{auto it=entries.find(key);return it==entries.end()?nullptr:it->second;}int add(const char*key,ObJsonNode*v){return entries.emplace(key,v).second?0:1;}};
struct ObAIFuncJsonUtils{
static int get_json_object(ObIAllocator&,ObJsonObject*&out){out=new ObJsonObject();return 0;}
static int get_json_string(ObIAllocator&,ObString&v,ObJsonString*&out){out=new ObJsonString(v.text());return 0;}
static int get_json_boolean(ObIAllocator&,bool v,ObJsonBoolean*&out){out=new ObJsonBoolean(v);return 0;}
static int compact_json_object(ObIAllocator&,ObJsonObject*src,ObJsonObject*dst){if(src){for(auto&[k,v]:src->entries){if(dst->add(k.c_str(),v))return 1;}}return 0;}
};
struct ObOllamaUtils{struct ObOllamaComplete{static int get_body(ObIAllocator&,ObString&,ObString&,ObString&,ObJsonObject*,ObJsonObject*&);};};
namespace common{using ObIAllocator=::ObIAllocator;using ObString=::ObString;using ObJsonObject=::ObJsonObject;}
'''
main = r'''
int checks=0;
void yes(bool b){if(!b){std::cerr<<"FAILED test "<<checks<<"\n";std::abort();}++checks;}
int main(){
 ObIAllocator a;ObString model("qwen2.5"),prompt(""),message("hello"),missing("");ObJsonObject*body=nullptr;
 yes(ObOllamaUtils::ObOllamaComplete::get_body(a,model,prompt,message,nullptr,body)==0);
 yes(static_cast<ObJsonString*>(body->get_value("model"))->value=="qwen2.5");
 yes(static_cast<ObJsonString*>(body->get_value("prompt"))->value=="hello");
 yes(body->get_value("stream")->json_type()==ObJsonNodeType::J_BOOLEAN);
 yes(!body->get_value("stream")->get_boolean());
 ObJsonObject valid;valid.add("stream",new ObJsonBoolean(false));body=nullptr;
 yes(ObOllamaUtils::ObOllamaComplete::get_body(a,model,prompt,message,&valid,body)==0);
 yes(!body->get_value("stream")->get_boolean());
 ObJsonObject streaming;streaming.add("stream",new ObJsonBoolean(true));body=nullptr;
 yes(ObOllamaUtils::ObOllamaComplete::get_body(a,model,prompt,message,&streaming,body)==OB_INVALID_ARGUMENT);
 ObJsonObject wrong;wrong.add("stream",new ObJsonString("false"));body=nullptr;
 yes(ObOllamaUtils::ObOllamaComplete::get_body(a,model,prompt,message,&wrong,body)==OB_INVALID_ARGUMENT);
 yes(ObOllamaUtils::ObOllamaComplete::get_body(a,missing,prompt,message,nullptr,body)==OB_INVALID_ARGUMENT);
 std::cout<<"PASS: "<<checks<<" production Ollama checks\n";
}
'''
with tempfile.TemporaryDirectory() as d:
  src_file=Path(d)/'fixture.cpp';exe=Path(d)/'fixture'
  src_file.write_text(header+src[start:end]+main)
  subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-O2',str(src_file),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
