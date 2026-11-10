#!/usr/bin/env python3
"""Compile the production rerank result mapping and merge methods with test stubs."""
from __future__ import annotations
from pathlib import Path
import subprocess
import tempfile

source=(Path(__file__).resolve().parents[2]/'src/sql/engine/expr/ob_expr_ai/ob_expr_ai_rerank.cpp').read_text()
names=[
 ('int ObExprAIRerank::sort_document_array_by_model_result(', 'int ObExprAIRerank::construct_batch_document_array('),
 ('int ObExprAIRerank::batch_result_add_base(', 'int ObExprAIRerank::compact_json_array_by_key('),
 ('int ObExprAIRerank::compact_json_array_by_key(', 'int ObExprAIRerank::construct_config_json('),
]
code='\n'.join(source[source.index(start):source.index(end,source.index(start))] for start,end in names)
head=r'''
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>
#define OB_SUCCESS 0
#define OB_INVALID_ARGUMENT 1
#define OB_INVALID_DATA 2
#define OB_ERR_UNEXPECTED 3
#define OB_FAIL(x) ((ret=(x))!=OB_SUCCESS)
#define OB_SUCC(x) ((x)==OB_SUCCESS)
#define OB_ISNULL(x) ((x)==nullptr)
#define INIT_SUCC(ret) int ret=OB_SUCCESS
#define UNUSED(x) (void)(x)
struct ObIAllocator{};
struct ObString{std::string value;explicit ObString(std::string v):value(std::move(v)){}const char*ptr()const{return value.c_str();}};
enum class ObJsonNodeType{J_OBJECT,J_ARRAY,J_INT,J_DOUBLE,J_STRING};
struct ObJsonNode {virtual ObJsonNodeType json_type()const=0;virtual ObJsonNode*clone(ObIAllocator*,bool)const=0;virtual ~ObJsonNode()=default;};
struct ObJsonInt:ObJsonNode{int64_t x;explicit ObJsonInt(int64_t v):x(v){}ObJsonNodeType json_type()const override{return ObJsonNodeType::J_INT;}ObJsonNode*clone(ObIAllocator*,bool)const override{return new ObJsonInt(x);}int64_t value()const{return x;}void set_value(int64_t v){x=v;}};
struct ObJsonDouble:ObJsonNode{double x;explicit ObJsonDouble(double v):x(v){}ObJsonNodeType json_type()const override{return ObJsonNodeType::J_DOUBLE;}ObJsonNode*clone(ObIAllocator*,bool)const override{return new ObJsonDouble(x);}double value()const{return x;}};
struct ObJsonString:ObJsonNode{ObJsonNodeType json_type()const override{return ObJsonNodeType::J_STRING;}ObJsonNode*clone(ObIAllocator*,bool)const override{return new ObJsonString();}};
struct ObJsonObject:ObJsonNode{std::map<std::string,ObJsonNode*> members;ObJsonNodeType json_type()const override{return ObJsonNodeType::J_OBJECT;}ObJsonNode*clone(ObIAllocator*,bool)const override{return new ObJsonObject(*this);}ObJsonNode*get_value(const char*k)const{auto it=members.find(k);return it==members.end()?nullptr:it->second;}ObJsonNode*get_value(const ObString&k)const{return get_value(k.ptr());}int add(const char*k,ObJsonNode*val){return members.emplace(k,val).second?0:OB_INVALID_ARGUMENT;}};
struct ObJsonArray:ObJsonNode{std::vector<ObJsonNode*> items;ObJsonNodeType json_type()const override{return ObJsonNodeType::J_ARRAY;}ObJsonNode*clone(ObIAllocator*,bool)const override{return new ObJsonArray(*this);}int64_t element_count()const{return items.size();}ObJsonNode*get_value(int64_t n)const{return items.at(n);}int append(ObJsonNode*e){items.push_back(e);return 0;}};
template<typename T>struct ObArray{std::deque<T> items;int push_back(T v){items.push_back(v);return 0;}T&at(int64_t i){return items.at(i);}};
struct ObAIFuncJsonUtils{static int get_json_array(ObIAllocator&,ObJsonArray*&out){out=new ObJsonArray();return 0;}};
struct ObExprAIRerank{
static constexpr char MODEL_SCORE_KEY[20]="_model_score";
static constexpr char INDEX_KEY[20]="index";
static constexpr char SCORE_KEY[20]="relevance_score";
static int sort_document_array_by_model_result(ObIAllocator&,ObJsonArray*,ObJsonArray*,ObJsonArray*&);
static int batch_result_add_base(ObIAllocator&,ObJsonArray*,int64_t);
static int compact_json_array_by_key(ObIAllocator&,ObJsonArray*,ObJsonArray*,ObString&,ObJsonArray*&);
};
'''
main=r'''
int checks=0;void yes(bool v){if(!v){std::cerr<<"FAILED case "<<checks<<"\n";std::abort();}++checks;}
ObJsonObject* scored(int64_t id,double value){auto*o=new ObJsonObject();o->add("index",new ObJsonInt(id));o->add("relevance_score",new ObJsonDouble(value));return o;}
ObJsonArray* rows(std::initializer_list<ObJsonNode*> xs){auto*a=new ObJsonArray();a->items=xs;return a;}
double score(ObJsonNode*x){return static_cast<ObJsonDouble*>(static_cast<ObJsonObject*>(x)->get_value("relevance_score"))->value();}
int main(){
 ObIAllocator alloc;ObString key("relevance_score");ObJsonArray*result=nullptr;
 ObJsonArray*left=rows({scored(0,.95),scored(1,.55),scored(2,.45),scored(3,.30)});
 ObJsonArray*right=rows({scored(4,.70)});
 yes(ObExprAIRerank::compact_json_array_by_key(alloc,left,right,key,result)==0);
 yes(result->element_count()==5);
 yes(score(result->get_value(0))==.95 && score(result->get_value(1))==.70);
 yes(score(result->get_value(2))==.55 && score(result->get_value(3))==.45 && score(result->get_value(4))==.30);
 left=rows({scored(0,.7),scored(1,.3)});right=rows({scored(2,.7),scored(3,.3)});
 yes(ObExprAIRerank::compact_json_array_by_key(alloc,left,right,key,result)==0);
 yes(static_cast<ObJsonInt*>(static_cast<ObJsonObject*>(result->get_value(0))->get_value("index"))->value()==0);
 yes(static_cast<ObJsonInt*>(static_cast<ObJsonObject*>(result->get_value(2))->get_value("index"))->value()==1);
 yes(ObExprAIRerank::compact_json_array_by_key(alloc,rows({}),right,key,result)==0);
 yes(result->element_count()==2);
 ObJsonArray*batch=rows({scored(1,.8),scored(0,.7)});
 yes(ObExprAIRerank::batch_result_add_base(alloc,batch,20)==0);
 yes(static_cast<ObJsonInt*>(static_cast<ObJsonObject*>(batch->get_value(0))->get_value("index"))->value()==21);
 yes(static_cast<ObJsonInt*>(static_cast<ObJsonObject*>(batch->get_value(1))->get_value("index"))->value()==20);
 yes(ObExprAIRerank::batch_result_add_base(alloc,rows({scored(1,.7)}),20)==OB_INVALID_DATA);
 yes(ObExprAIRerank::batch_result_add_base(alloc,rows({scored(0,.7),scored(0,.6)}),0)==OB_INVALID_DATA);
 yes(ObExprAIRerank::batch_result_add_base(alloc,rows({scored(0,std::numeric_limits<double>::infinity())}),0)==OB_INVALID_DATA);
 ObJsonArray*docs=rows({scored(0,.5),scored(1,.3)});
 ObJsonArray*ranking=rows({scored(1,.9),scored(0,.1)});
 ObJsonArray*out=nullptr;
 yes(ObExprAIRerank::sort_document_array_by_model_result(alloc,docs,ranking,out)==0);
 yes(static_cast<ObJsonInt*>(static_cast<ObJsonObject*>(out->get_value(0))->get_value("index"))->value()==1);
 yes(static_cast<ObJsonDouble*>(static_cast<ObJsonObject*>(out->get_value(0))->get_value("_model_score"))->value()==.9);
 yes(ObExprAIRerank::sort_document_array_by_model_result(alloc,docs,rows({scored(0,.9),scored(0,.1)}),out)==OB_INVALID_DATA);
 yes(ObExprAIRerank::sort_document_array_by_model_result(alloc,docs,rows({scored(0,.9),scored(7,.1)}),out)==OB_INVALID_DATA);
 yes(ObExprAIRerank::sort_document_array_by_model_result(alloc,docs,rows({scored(0,.9)}),out)==OB_INVALID_DATA);
 std::cout<<"PASS: "<<checks<<" production rerank checks\n";
}
'''
with tempfile.TemporaryDirectory() as d:
  f=Path(d)/'fixture.cpp';executable=Path(d)/'fixture';f.write_text(head+code+main)
  subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-O2',str(f),'-o',str(executable)],check=True)
  subprocess.run([str(executable)],check=True)
