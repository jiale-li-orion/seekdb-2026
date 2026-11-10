#!/usr/bin/env python3
"""Compile the production SQL AI_EMBED batch callback with deterministic runtime stubs."""
from pathlib import Path
import subprocess
import tempfile

source=(Path(__file__).resolve().parents[2]/'src/sql/engine/expr/ob_expr_ai/ob_expr_ai_embed.cpp').read_text()
start=source.index('int ObExprAIEmbed::eval_ai_embed_batch(')
end=source.index('int ObExprAIEmbed::cg_expr(',start)
head=r'''
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <string>
#include <vector>
#include <utility>
#define OB_SUCCESS 0
#define OB_INVALID_ARGUMENT 1
#define OB_ERR_UNEXPECTED 2
#define OB_INVALID_DATA 3
#define OB_ALLOCATE_MEMORY_FAILED 4
#define OB_FAIL(x) ((ret = (x)) != OB_SUCCESS)
#define OB_SUCC(x) ((x) == OB_SUCCESS)
#define OB_ISNULL(x) ((x)==nullptr)
#define INIT_SUCC(ret) int ret = OB_SUCCESS
#define LOG_USER_ERROR(...) do {} while(0)
#define N_AI_EMBED "AI_EMBED"
struct ObString {
 std::string value;ObString()=default;explicit ObString(std::string v):value(std::move(v)){}
 bool empty()const{return value.empty();}bool operator==(const ObString&x)const{return value==x.value;}bool operator!=(const ObString&x)const{return value!=x.value;}
};
struct ObDatum {bool null=false;ObString text;int64_t number=0;bool is_null()const{return null;}
 ObString get_string()const{return text;}int64_t get_int()const{return number;}
 void set_null(){null=true;text=ObString();}void set_string(ObString x){null=false;text=std::move(x);}
};
struct ObBitVector {std::vector<bool> items;bool at(int64_t n)const{return items.at(n);}void set(int64_t n){items.at(n)=true;}};
struct ObIAllocator{};
struct ObDatumMeta {};
struct ObObjMeta {bool has_lob_header()const{return false;}};
struct ObExecContext {};
struct ObEvalCtx {ObExecContext exec_ctx_;int64_t batch_idx_=0;
 struct TempAllocGuard {ObIAllocator allocator;explicit TempAllocGuard(ObEvalCtx&){} ObIAllocator&get_allocator(){return allocator;}};
 struct BatchInfoScopeGuard{ObEvalCtx& ctx;explicit BatchInfoScopeGuard(ObEvalCtx&c):ctx(c){}void set_batch_size(int64_t){}void set_batch_idx(int64_t n){ctx.batch_idx_=n;}};
};
struct MultimodeAlloctor:ObIAllocator{explicit MultimodeAlloctor(ObIAllocator&){} };
namespace lib{struct ObMemAttr{explicit ObMemAttr(const char*){}};struct ObMallocHookAttrGuard{explicit ObMallocHookAttrGuard(ObMemAttr){}};}
template<typename T>struct ObArray {std::deque<T> xs;int push_back(T v){xs.push_back(v);return 0;}
 T&at(int64_t n){return xs.at(n);}const T&at(int64_t n)const{return xs.at(n);}
 bool empty()const{return xs.empty();}int64_t count()const{return xs.size();}};
struct ObJsonObject{int64_t dimension=0;int add(const char*name,void*val){if(std::string(name)=="dimensions")dimension=*static_cast<int64_t*>(val);return 0;}};
struct ObJsonInt{int64_t x;explicit ObJsonInt(int64_t v):x(v){}};
struct ObAIFuncExprInfo{ObString model;};
namespace oceanbase{namespace share{struct ObAiModelEndpointInfo{};}namespace query{
struct ObIAiEndpointResolver{int resolve_by_model_name(const ObString&,ObIAllocator&,share::ObAiModelEndpointInfo&) {return 0;}};
}namespace share{
template<typename T>T*server_service(){static T resolver;return &resolver;}
}}
struct ObExpr {int64_t arg_cnt_=2;ObExpr *args_[3]={};mutable ObDatum datum[12];mutable ObBitVector evaluated{{std::vector<bool>(12,false)}};
 ObDatumMeta datum_meta_; ObObjMeta obj_meta_;
 int eval_batch(ObEvalCtx&,const ObBitVector&,int64_t){return 0;}
 ObDatum*locate_batch_datums(ObEvalCtx&)const{return datum;}
 ObBitVector&get_evaluated_flags(ObEvalCtx&)const{return evaluated;}
};
struct ObTextStringHelper{static int read_real_string_data(ObExecContext&,ObIAllocator&,ObDatum&d,ObDatumMeta&,bool,ObString&result){result=d.get_string();return 0;}};
struct ObAIFuncJsonUtils{static int get_json_object(ObIAllocator&,ObJsonObject*&o){o=new ObJsonObject();return 0;}
static int get_json_int(ObIAllocator&,int64_t x,ObJsonInt*&o){o=new ObJsonInt(x);return 0;}};
int provider_calls=0;std::vector<int64_t> sizes;std::vector<std::string> model_labels;
struct ObAIFuncUtils{static int get_ai_func_info(ObIAllocator&,const ObString&name,ObAIFuncExprInfo*&o){o=new ObAIFuncExprInfo();o->model=name;return 0;}
static int set_string_result(const ObExpr&,ObEvalCtx&,ObDatum&out,ObString&text){out.set_string(text);return 0;}};
struct ObAIFuncModel{ObString id;explicit ObAIFuncModel(ObIAllocator&,ObAIFuncExprInfo&info,oceanbase::share::ObAiModelEndpointInfo&):id(info.model){}
int call_dense_embedding_vector_v2(ObArray<ObString>&contents,ObJsonObject*config,ObArray<ObString>&results){
 ++provider_calls;sizes.push_back(contents.count());model_labels.push_back(id.value);for(int64_t i=0;i<contents.count();++i){
  if(results.push_back(ObString(contents.at(i).value+":"+std::to_string(config?config->dimension:0)))!=0)return 1;
 }return 0;}};
struct ObExprAIEmbed{static constexpr int MODEL_IDX=0,CONTENT_IDX=1,DIM_IDX=2;
 static int eval_ai_embed_batch(const ObExpr&,ObEvalCtx&,const ObBitVector&,int64_t);};
using namespace oceanbase;
'''
main=r'''
void assert_that(bool b,const char*msg){if(!b){std::cerr<<msg<<"\n";std::abort();}}
int main(){
 ObEvalCtx ctx;ObExpr expr,model,input,dimension;expr.arg_cnt_=3;expr.args_[0]=&model;expr.args_[1]=&input;expr.args_[2]=&dimension;
 const char*models[]={"A","A","A","B","A","A","B","C"};
 const char*texts[]={"first","null","third","fourth","fifth","sixth","seventh","skipped"};
 int64_t dims[]={0,0,0,4,0,4,4,4};
 for(int i=0;i<8;++i){model.datum[i].text=ObString(models[i]);input.datum[i].text=ObString(texts[i]);dimension.datum[i].number=dims[i];}
 input.datum[1].null=true;
 ObBitVector skip{{false,false,false,false,false,false,false,true}};
 // The 0-valued dimension represents a missing parameter in a two-argument call.
 expr.arg_cnt_=2;
 int ret=ObExprAIEmbed::eval_ai_embed_batch(expr,ctx,skip,8);
 assert_that(ret==0,"initial batch failed");
 assert_that(provider_calls==2,"expected two grouped model requests");
 assert_that(sizes==std::vector<int64_t>({4,2}),"unexpected grouping sizes");
 assert_that(model_labels==std::vector<std::string>({"A","B"}),"wrong model grouping");
 assert_that(expr.datum[0].text.value=="first:0","wrong row 0");
 assert_that(expr.datum[1].is_null(),"NULL did not propagate");
 assert_that(expr.datum[2].text.value=="third:0","wrong row 2");
 assert_that(expr.datum[4].text.value=="fifth:0","wrong row 4");
 assert_that(expr.datum[7].text.value.empty(),"skipped row processed");
 // Explicit dimensions: adjacent rows with matching dim combine; other dim splits.
 provider_calls=0;sizes.clear();model_labels.clear();expr.evaluated.items.assign(12,false);expr.arg_cnt_=3;
 dimension.datum[0].number=4;dimension.datum[2].number=4;dimension.datum[4].number=6;
 ret=ObExprAIEmbed::eval_ai_embed_batch(expr,ctx,skip,8);
 assert_that(ret==0,"dimension batch failed");
 assert_that(provider_calls==3,"expected three model+dimension requests");
 assert_that(sizes==std::vector<int64_t>({3,2,1}),"dimensions were not grouped correctly");
 assert_that(model_labels==std::vector<std::string>({"A","B","A"}),"model/dimension grouping wrong");
 assert_that(expr.datum[0].text.value=="first:4"&&expr.datum[2].text.value=="third:4","dimension mapping wrong");
 assert_that(expr.datum[4].text.value=="fifth:6","dimension group result wrong");
 provider_calls=0;sizes.clear();model_labels.clear();expr.evaluated.items.assign(12,false);
 dimension.datum[1].number=4;dimension.datum[1].null=true;
 ret=ObExprAIEmbed::eval_ai_embed_batch(expr,ctx,skip,8);
 assert_that(ret==0&&expr.datum[1].is_null(),"NULL dimension propagation wrong");
 dimension.datum[1].null=false;model.datum[0].null=true;expr.evaluated.items.assign(12,false);
 assert_that(ObExprAIEmbed::eval_ai_embed_batch(expr,ctx,skip,8)==OB_INVALID_ARGUMENT,"NULL model accepted");
 model.datum[0].null=false;dimension.datum[0].number=-1;expr.evaluated.items.assign(12,false);
 assert_that(ObExprAIEmbed::eval_ai_embed_batch(expr,ctx,skip,8)==OB_INVALID_ARGUMENT,"negative dimension accepted");
 std::cout<<"PASS: grouped runtime batch checks (NULL, skip, dimensions, scatter)\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'fixture.cpp';exe=Path(d)/'fixture';f.write_text(head+source[start:end]+main)
 subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Wno-unused-parameter',str(f),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
