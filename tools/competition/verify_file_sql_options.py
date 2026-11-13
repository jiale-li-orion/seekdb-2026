#!/usr/bin/env python3
"""Compile production file SQL named-option AST helper with stand-in parse nodes."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "src/sql/resolver/dml/ob_dml_resolver.cpp").read_text()
start = source.index('namespace {\n\n// SQL grammar represents `header = true`')
end = source.index('\nint ObDMLResolver::resolve_file_sql_table_item(', start)
helper = source[start:end]
header = r'''
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <algorithm>
#define OB_SUCCESS 0
#define OB_ERR_PARSE_SQL 1
#define OB_INVALID_ARGUMENT 2
#define OB_NOT_SUPPORTED 3
#define OB_SUCC(v) ((v) == OB_SUCCESS)
enum ObItemType {T_EXPR_LIST,T_OP_EQ,T_COLUMN_REF,T_BOOL,T_VARCHAR,T_INT,T_OP_NEG};
struct ParseNode {
  ObItemType type_;
  int32_t num_child_ = 0;
  int64_t value_ = 0;
  const char* str_value_ = nullptr;
  int32_t str_len_ = 0;
  const ParseNode* children_[3] = {nullptr,nullptr,nullptr};
};
struct ObString {
  std::string value_;
  explicit ObString(const char* str):value_(str){}
  ObString(int length, const char* str):value_(str,length){}
  int case_compare(const ObString &other) const {
    std::string left=value_,right=other.value_;
    std::transform(left.begin(),left.end(),left.begin(),::tolower);
    std::transform(right.begin(),right.end(),right.begin(),::tolower);
    return left.compare(right);
  }
  int case_compare(const char *other) const {return case_compare(ObString(other));}
};
'''
body = r'''
ParseNode literal(ObItemType type,const char*name=nullptr,int64_t value=0) {
 ParseNode n{};n.type_=type;n.value_=value;n.str_value_=name;n.str_len_=name?std::char_traits<char>::length(name):0;return n;
}
ParseNode option(const ParseNode*key,const ParseNode*value){ParseNode n=literal(T_OP_EQ);n.num_child_=2;n.children_[0]=key;n.children_[1]=value;return n;}
ParseNode column(const ParseNode*name){ParseNode n=literal(T_COLUMN_REF);n.num_child_=3;n.children_[2]=name;return n;}
ParseNode args(const ParseNode*first,const ParseNode*one=nullptr,const ParseNode*two=nullptr){ParseNode n=literal(T_EXPR_LIST);n.children_[0]=first;n.num_child_=1;if(one){n.children_[1]=one;n.num_child_=2;}if(two){n.children_[2]=two;n.num_child_=3;}return n;}
int main(){
  bool header=false;
  auto path=literal(T_VARCHAR,"/data/file.csv");
  auto hdr=literal(T_VARCHAR,"header"),sample=literal(T_VARCHAR,"sample_size"),format=literal(T_VARCHAR,"format");
  auto hdr_col=column(&hdr),sample_col=column(&sample),format_col=column(&format);
  auto truth=literal(T_BOOL,nullptr,1),falsity=literal(T_BOOL,nullptr,0),positive=literal(T_INT,nullptr,1),ten=literal(T_INT,nullptr,10);
  auto neg=literal(T_OP_NEG);neg.num_child_=1;neg.children_[0]=&positive;
  auto hdr_yes=option(&hdr_col,&truth),hdr_no=option(&hdr_col,&falsity),all=option(&sample_col,&neg),few=option(&sample_col,&ten);
  auto json_type=literal(T_VARCHAR,"newline_delimited");auto json_format=option(&format_col,&json_type);
  auto a=args(&path,&hdr_yes,&all);
  if(resolve_file_sql_options(a,1,header)!=0||!header)std::abort();
  a=args(&path,&hdr_no);if(resolve_file_sql_options(a,1,header)!=0||header)std::abort();
  a=args(&path,&few);if(resolve_file_sql_options(a,1,header)!=OB_NOT_SUPPORTED)std::abort();
  a=args(&path,&hdr_no,&hdr_yes);if(resolve_file_sql_options(a,1,header)!=OB_INVALID_ARGUMENT)std::abort();
  a=args(&path,&json_format);if(resolve_file_sql_options(a,2,header)!=0)std::abort();
  a=args(&path,&hdr_yes);if(resolve_file_sql_options(a,2,header)!=OB_INVALID_ARGUMENT)std::abort();
  a=args(&path,&json_format);if(resolve_file_sql_options(a,1,header)!=OB_NOT_SUPPORTED)std::abort();
  a=args(&path,&json_type);if(resolve_file_sql_options(a,1,header)!=OB_ERR_PARSE_SQL)std::abort();
  std::cout<<"PASS: 8 production option parsing scenarios\n";
}
'''
with tempfile.TemporaryDirectory() as d:
  path=Path(d)/"fixture.cpp"
  binary=Path(d)/"fixture"
  path.write_text(header+helper+body)
  subprocess.run(["g++","-std=c++17","-O2","-Wall","-Wextra","-Werror",str(path),"-o",str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
