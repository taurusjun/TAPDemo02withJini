#include <iostream>
#include <boost/algorithm/string.hpp>
using namespace std;

int main()
{
  cout <<"-------- main test --------" << endl;
  string line = "CFFEX:F:IF:1910:A"; 
  std::vector<std::string> strs;
  boost::split(strs, line, boost::is_any_of(":"));
  for(int i=0;i<strs.size();i++){
    cout << strs.at(i) << endl;
  }
  cout <<"concation: "<< strs[2]+strs[3]<<endl;
  cout << strs[4].compare("A")<<endl;
  cout << strs[4].compare("a")<<endl;
  cout << strs[4].compare("b")<<endl;
  return 0;
}
