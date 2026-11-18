#include "ClapRuntime.h"
#include "ModelPack.h"
#include "AudioIPC.h"
#include "Performance.h"
#include "Catalog.h"
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
#include <pthread/qos.h>
namespace
{
juce::var perform(const juce::var &request, std::unique_ptr<sp::ClapRuntime> &runtime)
{
    auto reply = sp::message("ai-result"); auto *out = reply.getDynamicObject();
    out->setProperty("workerPid",(int)getpid());
    out->setProperty("sequence",request["sequence"]);
    try
    {
        const auto command = request["command"].toString();
        pthread_set_qos_class_self_np(command=="text" ? QOS_CLASS_USER_INITIATED : QOS_CLASS_UTILITY,0);
        if (command == "load")
        {
            auto began = sp::monotonicMs();
            juce::String error;
            juce::File folder(request["modelDirectory"].toString());
            if (!sp::verifyModelPack(folder,error)) throw std::runtime_error(error.toStdString());
            runtime = std::make_unique<sp::ClapRuntime>(folder);
            out->setProperty("loadMs",sp::monotonicMs()-began);
        }
        else if (command == "status")
        {
            if (!runtime) throw std::runtime_error("AI model is not loaded");
            out->setProperty("ready",true);
        }
        else
        {
            if (!runtime) throw std::runtime_error("AI model is not loaded");
            std::vector<float> vector;
            if (command == "text")
            {
                vector = runtime->text(request["text"].toString());
                juce::Array<juce::var> tokens;
                for (auto id : runtime->tokenize(request["text"].toString())) tokens.add(id);
                out->setProperty("tokens",tokens);
            }
            else if (command == "audio")
            {
                juce::File file(request["path"].toString());
                sp::FileStamp before, after;
                auto expected = [&](const sp::FileStamp &s) {
                    return s.size == (int64_t)request["size"] && s.modified == (int64_t)request["modified"] &&
                           s.inode == (int64_t)request["inode"]; };
                if (!sp::fileStamp(file,before) || !expected(before))
                { out->setProperty("staleSource",true); throw std::runtime_error("Source offline or changed before analysis"); }
                vector = runtime->audio(file,(int64_t)request["startSample"],(int64_t)request["endSample"],
                    (double)request["sourceRate"],(int64_t)request["sourceSamples"]);
                if (!sp::fileStamp(file,after) || !expected(after))
                { out->setProperty("staleSource",true); throw std::runtime_error("Source changed during analysis"); }
            }
            else throw std::runtime_error("Unknown local AI command");
            juce::Array<juce::var> values;
            for (float x : vector) values.add((double)x);
            out->setProperty("embedding",values);
            out->setProperty("preprocessMs",runtime->lastPreprocessMs);
            out->setProperty("inferenceMs",runtime->lastInferenceMs);
        }
        out->setProperty("model",sp::modelId()); out->setProperty("preprocessing",sp::preprocessingId());
        out->setProperty("peakResidentBytes",sp::performance.json()["peakResidentBytes"]);
    }
    catch (const std::exception &e) { out->setProperty("error",juce::String::fromUTF8(e.what())); }
    return reply;
}
}
int main(int argc, char **argv)
{
    if (argc >= 4 && juce::String(argv[1]) == "--features")
    {
        juce::MemoryBlock bytes;
        if (!juce::File(argv[2]).loadFileAsData(bytes) || bytes.getSize()%sizeof(float) != 0) return 1;
        auto *raw = static_cast<const float *>(bytes.getData());
        auto result = sp::ClapRuntime::features(std::vector<float>(raw,raw+bytes.getSize()/sizeof(float)));
        return juce::File(argv[3]).replaceWithData(result.data(),result.size()*sizeof(float)) ? 0 : 1;
    }
    std::unique_ptr<sp::ClapRuntime> runtime;
    if (argc >= 3)
    {
        auto load = sp::message("load"); load.getDynamicObject()->setProperty("modelDirectory",juce::String(argv[2]));
        auto result = perform(load,runtime);
        if (result["error"].toString().isNotEmpty()) { std::cout << juce::JSON::toString(result).toStdString(); return 1; }
        if (juce::String(argv[1]) == "--text" && argc >= 4)
        {
            auto request = sp::message("text"); request.getDynamicObject()->setProperty("text",juce::String(argv[3]));
            auto reply = perform(request,runtime); reply.getDynamicObject()->setProperty("loadMs",result["loadMs"]);
            std::cout << juce::JSON::toString(reply).toStdString() << '\n';
            return reply["error"].toString().isEmpty() ? 0 : 1;
        }
        return 0;
    }
    int noSig = 1; setsockopt(STDIN_FILENO,SOL_SOCKET,SO_NOSIGPIPE,&noSig,sizeof(noSig));
    std::string input;
    for (;;)
    {
        char bytes[16384]; auto n = recv(STDIN_FILENO,bytes,sizeof(bytes),0);
        if (n <= 0) return 0;
        input.append(bytes,(size_t)n);
        if (input.size() > 65536) return 2;
        for (auto newline = input.find('\n'); newline != std::string::npos; newline = input.find('\n'))
        {
            auto request = juce::JSON::parse(juce::String::fromUTF8(input.data(),(int)newline));
            input.erase(0,newline+1);
            if ((int)request["version"] != sp::protocolVersion) return 2;
            if (!sp::sendMessage(STDIN_FILENO,perform(request,runtime))) return 2;
        }
    }
}
