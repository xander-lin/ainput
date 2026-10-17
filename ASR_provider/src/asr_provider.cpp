#include "asr_provider.h"

namespace ainput {

AsrProviderRegistry &AsrProviderRegistry::instance() {
    static AsrProviderRegistry reg;
    return reg;
}

void AsrProviderRegistry::registerFactory(
    std::unique_ptr<IAsrProviderFactory> factory) {
    factories_.push_back(std::move(factory));
}

std::vector<std::pair<std::string, std::string>>
AsrProviderRegistry::listFactories() const {
    std::vector<std::pair<std::string, std::string>> list;
    for (auto &f : factories_) {
        list.emplace_back(f->id(), f->displayName());
    }
    return list;
}

std::unique_ptr<IAsrProvider>
AsrProviderRegistry::create(const std::string &id) const {
    for (auto &f : factories_) { //遍历算法足够了
        if (f->id() == id) {
            return f->create();
        }
    }
    return nullptr;
}

} // namespace ainput
