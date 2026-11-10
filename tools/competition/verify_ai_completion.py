#!/usr/bin/env python3
"""Compile production OpenAI completion validation and JSON config type checks."""
from pathlib import Path
import subprocess
import tempfile

src = (Path(__file__).resolve().parents[2] / 'src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp').read_text()
def section(first,last):
  return src[src.index(first):src.index(last,src.index(first))]
code = section('int ObOpenAIUtils::ObOpenAIComplete::parse_output(', 'int ObOpenAIUtils::ObOpenAIEmbed::get_header(')
code += section('int ObAIFuncJsonUtils::get_json_object_form_str(', 'int ObAIFuncJsonUtils::compact_json_object(')
head = r'''
#include <string>
#include <vector>
#include <iostream>
#include <cstdlib>
#define OB_SUCCESS 0
#define OB_INVALID_ARGUMENT 1
#define OB_INVALID_DATA 2
#define OB_FAIL(x) ((ret=(x)) != OB_SUCCESS)
#define OB_ISNULL(x) ((x)==nullptr)
struct ObIAllocator{};
struct ObString{std::string value;explicit ObString(std::string v):value(v){}bool empty()const{return value.empty();}};
enum class ObJsonNodeType {J_OBJECT,J_ARRAY,J_STRING,J_NULL};
struct ObIJsonBase;
struct ObJsonSeekResult {std::vector<ObIJsonBase*> hits;size_t size()const{return hits.size();}ObIJsonBase*operator[](size_t i)const{return hits.at(i);}};
struct ObJsonPath{ObJsonPath(ObString&,ObIAllocator*){}int parse_path(){return 0;}int path_node_cnt()const{return 1;}};
struct ObIJsonBase {virtual ObJsonNodeType json_type()const=0;virtual int seek(ObJsonPath&,int,bool,bool,ObJsonSeekResult&){return 0;}virtual ~ObIJsonBase()=default;};
struct ObJsonString:ObIJsonBase{ObString value;explicit ObJsonString(std::string s):value(s){}ObJsonNodeType json_type()const override{return ObJsonNodeType::J_STRING;}const ObString&get_str()const{return value;}};
struct ObJsonArray:ObIJsonBase{ObJsonNodeType json_type()const override{return ObJsonNodeType::J_ARRAY;}};
struct ObJsonObject:ObIJsonBase{ObIJsonBase*content=nullptr;ObJsonNodeType json_type()const override{return ObJsonNodeType::J_OBJECT;}int seek(ObJsonPath&,int,bool,bool,ObJsonSeekResult&out)override{if(content)out.hits.push_back(content);return 0;}};
enum class ObJsonInType{JSON_TREE};
struct ObJsonBaseFactory{static int get_json_base(ObIAllocator*,ObString&str,ObJsonInType,ObJsonInType,ObIJsonBase*&out){if(str.value=="{}")out=new ObJsonObject();else if(str.value=="[]")out=new ObJsonArray();else return OB_INVALID_ARGUMENT;return 0;}};
struct ObOpenAIUtils{struct ObOpenAIComplete{static int parse_output(ObIAllocator&,ObJsonObject*,ObIJsonBase*&);};};
struct ObAIFuncJsonUtils{static int get_json_object_form_str(ObIAllocator&,ObString&,ObJsonObject*&);};
namespace common{using ObIAllocator=::ObIAllocator;using ObJsonObject=::ObJsonObject;using ObIJsonBase=::ObIJsonBase;using ObString=::ObString;}
'''
main = r'''
int count=0;void yes(bool v){if(!v){std::cerr<<"FAIL case "<<count<<"\n";std::abort();}++count;}
int main(){ObIAllocator a;ObJsonObject r;ObIJsonBase*out=nullptr;
ObJsonString ok("done");r.content=&ok;
yes(ObOpenAIUtils::ObOpenAIComplete::parse_output(a,&r,out)==0 && out==&ok);
ObJsonString empty("");r.content=&empty;
yes(ObOpenAIUtils::ObOpenAIComplete::parse_output(a,&r,out)==OB_INVALID_DATA);
ObJsonArray wrong;r.content=&wrong;
yes(ObOpenAIUtils::ObOpenAIComplete::parse_output(a,&r,out)==OB_INVALID_DATA);
r.content=nullptr;yes(ObOpenAIUtils::ObOpenAIComplete::parse_output(a,&r,out)==OB_INVALID_DATA);
yes(ObOpenAIUtils::ObOpenAIComplete::parse_output(a,nullptr,out)==OB_INVALID_ARGUMENT);
ObString obj("{}"),arr("[]"),bad("not-json");ObJsonObject *config=nullptr;
yes(ObAIFuncJsonUtils::get_json_object_form_str(a,obj,config)==OB_SUCCESS && config!=nullptr);
yes(ObAIFuncJsonUtils::get_json_object_form_str(a,arr,config)==OB_INVALID_ARGUMENT);
yes(ObAIFuncJsonUtils::get_json_object_form_str(a,bad,config)==OB_INVALID_ARGUMENT);
std::cout<<"PASS: "<<count<<" production completion checks\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'fixture.cpp'; executable=Path(d)/'fixture'; f.write_text(head+code+main)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-O2',str(f),'-o',str(executable)],check=True)
 subprocess.run([str(executable)],check=True)
