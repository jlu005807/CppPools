#include "cpppools/db/mock_score_store.h"

#include <chrono>
#include <mutex>

namespace cpppools::db {

namespace {
// mock 模式下的 updated_at 取值点。
// 用 system_clock 而非 steady_clock：这个值要作为墙上时间暴露给客户端，
// 语义上必须可与外部时间比对，因此不能用只保证单调、无固定纪元的 steady_clock。
long long now_epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
}  // namespace

ScoreResult MockScoreStore::create(int user_id, int score) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (records_.find(user_id) != records_.end()) {
        // create 不做 upsert：已存在时保持原值不动，由上层映射成 409。
        return ScoreResult::already_exists();
    }
    const ScoreRecord rec{user_id, score, now_epoch_ms()};
    records_.emplace(user_id, rec);
    return ScoreResult::success(rec);
}

ScoreResult MockScoreStore::get(int user_id) const {
    // 读路径用 shared_lock：多个 get 可并发进入，互不阻塞。
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = records_.find(user_id);
    if (it == records_.end()) {
        return ScoreResult::not_found();
    }
    return ScoreResult::success(it->second);
}

ScoreResult MockScoreStore::update(int user_id, int score) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto it = records_.find(user_id);
    if (it == records_.end()) {
        // 不存在时不插入：update 与 create 的职责严格分离。
        return ScoreResult::not_found();
    }
    it->second.score = score;
    it->second.updated_at_ms = now_epoch_ms();
    return ScoreResult::success(it->second);
}

ScoreResult MockScoreStore::remove(int user_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto it = records_.find(user_id);
    if (it == records_.end()) {
        return ScoreResult::not_found();
    }
    // 先取快照再擦除：erase 之后迭代器失效，快照要在那之前拿到。
    const ScoreRecord removed = it->second;
    records_.erase(it);
    return ScoreResult::success(removed);
}

std::size_t MockScoreStore::size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return records_.size();
}

}  // namespace cpppools::db
