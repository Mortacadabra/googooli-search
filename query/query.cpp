// Build:  make -C query
// Run:    ./query/query [project_root]      (default ".")
// Input:  one query per line: "and <terms>", "or <terms>" or "debug <terms>". quits on empty line


#include <algorithm>
#include <cstdio>
#include <iostream>
#include <memory>
#include <queue>
#include <string>
#include <vector>

#include "../indexer/tokenizer.h"
#include "../indexer/util.h"
#include "list_cursor.h"

static const size_t TOP_K = 10;

// for debugging: prints ft and the first few postings of a term
static void debug_list(const Index& ix, const std::string& term) {
    auto lp = openList(ix, term);
    if (!lp) {
        std::printf("%s: not found\n", term.c_str());
        return;
    }
    std::printf("%s: ft %u\n", term.c_str(), lp->ft);
    uint32_t d = lp->cur;
    for (int i = 0; i < 5 && d != END; ++i) {
        std::printf("  docID %u score %.4f\n", d, getScore(*lp));
        d = nextGEQ(*lp, d + 1);
    }
}

using Result = std::pair<double, uint32_t>;  // (score, docID)
using Lists = std::vector<std::unique_ptr<ListCursor>>;

// min heap keeping the TOP_K best results
struct TopK {
    std::priority_queue<Result, std::vector<Result>, std::greater<Result>> heap;

    void push(uint32_t doc, double score) {
        if (heap.size() == TOP_K && score <= heap.top().first) {  // most docs stop here
            return;
        }
        heap.push({score, doc});
        if (heap.size() > TOP_K) {
            heap.pop();
        }
    }

    // a doc has to score above this to get in to top k results
    double maxscore_threshold() const {
        if (heap.size() < TOP_K) {
            return 0;
        }
        return heap.top().first;
    }

    // best result first
    std::vector<Result> sorted() {
        std::vector<Result> out;
        for (; !heap.empty(); heap.pop()) {
            out.push_back(heap.top());
        }
        std::reverse(out.begin(), out.end());
        return out;
    }
};

// conjunctive query
static std::vector<Result> conjunctive(Lists& lists) {
    TopK top;
    std::sort(lists.begin(), lists.end(), [](auto& a, auto& b) { return a->ft < b->ft; });
    uint32_t did = 0;
    while ((did = nextGEQ(*lists[0], did)) != END) {
        uint32_t d = did;
        for (size_t i = 1; i < lists.size(); ++i) {
            d = nextGEQ(*lists[i], did);
            if (d != did) {
                break;
            }
        }
        if (d != did) { 
            if (d == END) {
                break;
            }
            did = d;
            continue;
        }
        double s = 0;
        for (auto& lp : lists) {
            s += getScore(*lp);
        }
        top.push(did, s);
        ++did;
    }
    return top.sorted();
}

static std::vector<Result> disjunctive(Lists& lists) {
    TopK top;
    std::sort(lists.begin(), lists.end(), [](auto& a, auto& b) { return a->max_score < b->max_score; });
    size_t n = lists.size();
    std::vector<double> prefix(n);  // prefix[i] = sum of max_score of lists 0..i
    for (size_t i = 0; i < n; ++i) {
        prefix[i] = lists[i]->max_score;
        if (i > 0) {
            prefix[i] += prefix[i - 1];
        }
    }

    size_t e = 0;
    while (true) {
        double threshold = top.maxscore_threshold();
        while (e < n && prefix[e] <= threshold) {
            ++e;
        }
        if (e == n) {  // no doc can surpass the threshold
            break;
        }

        uint32_t did = END;
        for (size_t i = e; i < n; ++i) {
            did = std::min(did, lists[i]->cur);
        }
        if (did == END) {
            break;
        }

        double s = 0;
        for (size_t i = e; i < n; ++i) {
            if (lists[i]->cur == did) {
                s += getScore(*lists[i]);
                nextGEQ(*lists[i], did + 1);
            }
        }
        for (size_t i = e; i-- > 0;) {  // non-essential lists
            if (s + prefix[i] <= threshold) { 
                break;
            }
            if (nextGEQ(*lists[i], did) == did) {
                s += getScore(*lists[i]);
            }
        }
        top.push(did, s);
    }
    return top.sorted();
}

static void run_query(const Index& ix, const std::string& text, bool conj) {
    double start = now_seconds();
    std::vector<std::string> terms = tokenize(text);
    std::sort(terms.begin(), terms.end());
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());

    Lists lists;
    bool missing = false;
    for (const std::string& t : terms) {
        if (auto lp = openList(ix, t)) {
            lists.push_back(std::move(lp));
        } else {
            missing = true;
        }
    }

    // handle misisng terms: AND returns no results and OR ignores it
    std::vector<Result> results;
    if (!lists.empty() && !(conj && missing)) {
        if (conj) {
            results = conjunctive(lists);
        } else {
            results = disjunctive(lists);
        }
    }

    std::printf("%zu results in %.2f ms\n", results.size(), (now_seconds() - start) * 1000);
    for (size_t r = 0; r < results.size(); ++r) {
        std::printf("%2zu. pid %-8u score %.4f\n", r + 1, ix.pid[results[r].second], results[r].first);
    }
}

int main(int argc, char** argv) {
    Index ix;
    std::string root = ".";
    if (argc > 1) {
        root = argv[1];
    }
    ix.load(root + "/output");
    std::printf("Loaded %zu terms, %zu documents.\n", ix.lexicon.size(), ix.pid.size());

    bool conj = false;
    std::string line;
    while (true) {
        std::printf("> ");
        std::fflush(stdout);
        if (!std::getline(std::cin, line) || line.empty()) {
            break;
        }
        if (line.rfind("debug ", 0) == 0) {
            std::vector<std::string> terms = tokenize(line.substr(6));
            for (const std::string& t : terms) {
                debug_list(ix, t);
            }
            continue;
        }
        if (line.rfind("and ", 0) == 0) {
            conj = true;
            line.erase(0, 4);
        } else if (line.rfind("or ", 0) == 0) {
            conj = false;
            line.erase(0, 3);
        }
        run_query(ix, line, conj);
    }
    return 0;
}
