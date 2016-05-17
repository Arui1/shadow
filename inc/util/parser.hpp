#ifndef SHADOW_PARSER_HPP
#define SHADOW_PARSER_HPP

#include "json.h"
#include "network.hpp"

#include <string>
#include <vector>

class Parser {
public:
  void ParseNetworkCfg(Network *net, std::string cfgfile, int batch = 1);
  void LoadWeights(Network *net, std::string weightfile);
  static void LoadImageList(std::vector<std::string> *imagelist,
                            std::string listfile);

private:
  void ParseNet(Network *net, Json::Value section);
  DataLayer *ParseData(Json::Value section, SizeParams params);
  ConvLayer *ParseConvolutional(Json::Value section, SizeParams params);
  PoolingLayer *ParsePooling(Json::Value section, SizeParams params);
  ConnectedLayer *ParseConnected(Json::Value section, SizeParams params);
  DropoutLayer *ParseDropout(Json::Value section, SizeParams params);

  void LoadWeightsUpto(Network *net, std::string weightfile, int cutoff);
};

#endif // SHADOW_PARSER_HPP