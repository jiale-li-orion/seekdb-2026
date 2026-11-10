#!/usr/bin/env python3
"""Compile production embedding validation and OpenAI index mapping with stubs.

This covers algorithmic error paths, independent of the unpatched server binary.
Full end-to-end mysqltest and build validation remain separate checks.
"""
from __future__ import annotations

import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / 'src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp').read_text()
parser_start = source.index('int ObOpenAIUtils::ObOpenAIEmbed::parse_output(')
parser_end = source.index('int ObOllamaUtils::get_header(', parser_start)
validate_start = source.index('int validate_ai_embedding_vector(')
validate_end = source.index('\n} // namespace\n\nint ObAIFuncModel::call_dense_embedding_vector_v2(', validate_start)

head = r'''
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#define OB_SUCCESS 0
#define OB_INVALID_ARGUMENT 1
#define OB_INVALID_DATA 2
#define OB_FAIL(x) ((ret = (x)) != OB_SUCCESS)
#define OB_SUCC(x) ((x) == OB_SUCCESS)
#define OB_ISNULL(x) ((x)==nullptr)
struct ObIAllocator {};
enum class ObJsonNodeType { J_ARRAY, J_OBJECT, J_INT, J_UINT, J_DOUBLE, J_OFLOAT, J_DECIMAL, J_STRING, J_NULL };
struct ObIJsonBase { virtual ObJsonNodeType json_type() const=0; virtual ~ObIJsonBase()=default; };
struct ObJsonNode : ObIJsonBase {};
struct ObJsonInt : ObJsonNode {int64_t v; explicit ObJsonInt(int64_t v):v(v){} ObJsonNodeType json_type() const override{return ObJsonNodeType::J_INT;} int64_t get_int()const{return v;}};
struct ObJsonUint : ObJsonNode {ObJsonNodeType json_type()const override{return ObJsonNodeType::J_UINT;}};
struct ObJsonDecimal : ObJsonNode {ObJsonNodeType json_type()const override{return ObJsonNodeType::J_DECIMAL;}};
struct ObJsonDouble : ObJsonNode {double v; explicit ObJsonDouble(double v):v(v){} double value()const{return v;} ObJsonNodeType json_type()const override{return ObJsonNodeType::J_DOUBLE;}};
struct ObJsonOFloat : ObJsonNode {float v; explicit ObJsonOFloat(float v):v(v){} float value()const{return v;} ObJsonNodeType json_type()const override{return ObJsonNodeType::J_OFLOAT;}};
struct ObJsonString : ObJsonNode {ObJsonNodeType json_type()const override{return ObJsonNodeType::J_STRING;}};
struct ObJsonArray : ObJsonNode {std::vector<ObJsonNode*> values; ObJsonNodeType json_type()const override{return ObJsonNodeType::J_ARRAY;} int64_t element_count()const{return values.size();} ObJsonNode*get_value(int64_t i)const{return values.at(i);} int append(ObJsonNode*n){values.push_back(n);return 0;}};
struct ObJsonObject : ObJsonNode {std::unordered_map<std::string,ObJsonNode*> values; ObJsonNodeType json_type()const override{return ObJsonNodeType::J_OBJECT;} ObJsonNode*get_value(const char*key)const{auto it=values.find(key);return it==values.end()?nullptr:it->second;}};
template<typename T> struct ObArray {std::vector<T> values; int push_back(T v){values.push_back(v);return 0;} int64_t count()const{return values.size();} T& at(int64_t i){return values.at(i);} };
struct ObAIFuncJsonUtils {static int get_json_array(ObIAllocator&, ObJsonArray*& out){out=new ObJsonArray();return 0;}};
struct ObOpenAIUtils {struct ObOpenAIEmbed{static int parse_output(ObIAllocator&,ObJsonObject*,ObIJsonBase*&);};};
namespace common {using ObIAllocator=::ObIAllocator;using ObJsonObject=::ObJsonObject;using ObIJsonBase=::ObIJsonBase;}
'''
main = r'''
int checks=0;
void yes(bool value){if (!value){std::cerr<<"FAIL at case "<<checks<<"\n"; std::abort();}++checks;}
ObJsonObject* make_embedding(int64_t index, ObJsonNode*vector, bool indexed=true){
 auto*obj=new ObJsonObject(); obj->values["embedding"]=vector;
 if(indexed) obj->values["index"]=new ObJsonInt(index);
 return obj;
}
ObJsonArray* vec(std::initializer_list<ObJsonNode*> elems){auto*v=new ObJsonArray();v->values=elems;return v;}
int main(){
 ObJsonInt i(2); ObJsonDouble d(1.25); ObJsonOFloat f(.5f); ObJsonUint u; ObJsonDecimal m; ObJsonString bad;
 yes(validate_ai_embedding_vector(vec({&i,&d,&f,&u,&m}), 5)==OB_SUCCESS);
 yes(validate_ai_embedding_vector(vec({&i,&d}), 0)==OB_SUCCESS);
 yes(validate_ai_embedding_vector(vec({&i,&d}), 3)==OB_INVALID_DATA);
 yes(validate_ai_embedding_vector(vec({}), 0)==OB_INVALID_DATA);
 yes(validate_ai_embedding_vector(vec({&bad}), 0)==OB_INVALID_DATA);
 yes(validate_ai_embedding_vector(vec({nullptr}), 0)==OB_INVALID_DATA);
 yes(validate_ai_embedding_vector(&bad, 0)==OB_INVALID_DATA);
 ObJsonDouble nan(std::numeric_limits<double>::quiet_NaN());ObJsonDouble inf(std::numeric_limits<double>::infinity());
 ObJsonOFloat finf(std::numeric_limits<float>::infinity());
 yes(validate_ai_embedding_vector(vec({&nan}), 0)==OB_INVALID_DATA);
 yes(validate_ai_embedding_vector(vec({&inf}), 0)==OB_INVALID_DATA);
 yes(validate_ai_embedding_vector(vec({&finf}), 0)==OB_INVALID_DATA);
 ObIAllocator allocator;
 auto*first=vec({&i}); auto*second=vec({&d});
 ObJsonObject response;response.values["data"]=vec({make_embedding(1,second),make_embedding(0,first)});
 ObIJsonBase*result=nullptr;
 yes(ObOpenAIUtils::ObOpenAIEmbed::parse_output(allocator,&response,result)==OB_SUCCESS);
 yes(static_cast<ObJsonArray*>(result)->get_value(0)==first && static_cast<ObJsonArray*>(result)->get_value(1)==second);
 auto fail=[&](std::initializer_list<ObJsonNode*> input){ObJsonObject r;r.values["data"]=vec(input);ObIJsonBase*out=nullptr;return ObOpenAIUtils::ObOpenAIEmbed::parse_output(allocator,&r,out);};
 yes(fail({make_embedding(0,first),make_embedding(0,second)})==OB_INVALID_DATA);
 yes(fail({make_embedding(0,first),make_embedding(5,second)})==OB_INVALID_DATA);
 yes(fail({make_embedding(0,first),make_embedding(1,second,false)})==OB_INVALID_DATA);
 yes(fail({make_embedding(0,&bad)})==OB_INVALID_DATA);
 yes(fail({make_embedding(0,first,false),make_embedding(1,second,false)})==OB_SUCCESS);
 ObJsonObject wrong;wrong.values["data"]=&bad;ObIJsonBase*out=nullptr;
 yes(ObOpenAIUtils::ObOpenAIEmbed::parse_output(allocator,&wrong,out)==OB_INVALID_DATA);
 std::cout<<"PASS: "<<checks<<" production embedding checks\n";
}
'''
with tempfile.TemporaryDirectory() as d:
    fixture=pathlib.Path(d)/'fixture.cpp'
    executable=pathlib.Path(d)/'fixture'
    fixture.write_text(head + source[parser_start:parser_end] + source[validate_start:validate_end] + main)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-O2',str(fixture),'-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True)
