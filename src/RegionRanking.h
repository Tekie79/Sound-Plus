#pragma once
#include "Catalog.h"
#include <algorithm>
#include <cmath>
namespace sp
{
// Scoring visits contiguous windows per asset. Rank recordings by their best window,
// retaining 32 window candidates per recording for five representative moments.
// A long recording cannot consume a global window budget and hide alternative recordings.
class RegionRanking
{
  public:
    explicit RegionRanking(int requested):limit(juce::jlimit(0,1000,requested)){current.windows.reserve(32);}
    void add(const juce::String &id,int64_t revision,int64_t first,int64_t last,float score)
    {
        if(!std::isfinite(score))return;
        if(current.id!=id){flush();current.id=id;current.revision=revision;}
        MatchRegion hit{first,last,score,1};auto &windows=current.windows;
        if(windows.size()==32 && (score<windows.back().similarity ||
            (score==windows.back().similarity && first>=windows.back().startSample)))return;
        auto at=std::lower_bound(windows.begin(),windows.end(),hit,[](const auto &a,const auto &b){return a.similarity!=b.similarity ? a.similarity>b.similarity : a.startSample<b.startSample;});
        windows.insert(at,hit);if(windows.size()>32)windows.pop_back();
    }
    std::vector<Asset> finish(Catalog &db,const juce::String &root)
    {
        flush();std::vector<Asset> result;
        for(auto &candidate:best)
        {
            auto a=db.asset(candidate.id);
            if(a.id.isEmpty() || a.revision!=candidate.revision || a.state!="ready" || (!root.isEmpty() && a.rootId!=root))continue;
            a.matchReason="Sound similarity";
            for(auto &hit:candidate.windows)
            {
                bool overlaps=false;
                for(auto &match:a.matches)if(hit.startSample<match.endSample && hit.endSample>match.startSample)
                {++match.groupedWindows;overlaps=true;break;}
                if(!overlaps && a.matches.size()<5)a.matches.push_back(hit);
            }
            result.push_back(std::move(a));
        }
        return result;
    }
  private:
    struct Candidate {juce::String id;int64_t revision=0;std::vector<MatchRegion> windows;};
    Candidate current;
    std::vector<Candidate> best;
    int limit;
    static bool order(const Candidate &a,const Candidate &b)
    {return a.windows.front().similarity!=b.windows.front().similarity ? a.windows.front().similarity>b.windows.front().similarity : a.id<b.id;}
    void flush()
    {
        if(current.windows.empty())return;
        if(limit>0 && ((int)best.size()<limit || order(current,best.back())))
        {auto at=std::lower_bound(best.begin(),best.end(),current,order);best.insert(at,current);if((int)best.size()>limit)best.pop_back();}
        current.windows.clear();
    }
};
}
