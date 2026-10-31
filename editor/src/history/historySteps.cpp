#include "history/historySteps.h"

#include <unordered_map>
#include <utility>

HistorySteps::Text HistorySteps::text(const nlohmann::json& snapshot)
{
  return std::make_shared<const std::string>(snapshot.dump());
}

void HistorySteps::push(const nlohmann::json& before,
                        const nlohmann::json& after,
                        Selection selectionBefore,
                        Selection selectionAfter)
{
  for (const Step& step : m_redo)
    m_bytes -= step.bytes();
  m_redo.clear();

  // Edits usually follow on from each other: this one starts where the last
  // ended, and the two share that scene's text.
  Text beforeText = text(before);
  if (!m_undo.empty() && *m_undo.back().after == *beforeText)
    beforeText = m_undo.back().after;
  m_undo.push_back({std::move(beforeText), text(after), selectionBefore, selectionAfter});
  m_bytes += m_undo.back().bytes();
  trim();
}

void HistorySteps::pushAssetEdit(std::shared_ptr<const AssetEdit> edit, Selection selection)
{
  for (const Step& step : m_redo)
    m_bytes -= step.bytes();
  m_redo.clear();
  // No scene of its own: an empty one, shared.
  static const Text none = text(nullptr);
  m_undo.push_back({none, none, selection, selection, std::move(edit)});
  m_bytes += m_undo.back().bytes();
  trim();
}

void HistorySteps::amendLast(const nlohmann::json& after, Selection selectionAfter)
{
  if (m_undo.empty())
    return;
  Step& last = m_undo.back();
  m_bytes -= last.bytes();
  last.after = text(after);
  last.selectionAfter = selectionAfter;
  m_bytes += last.bytes();
  trim();
}

std::optional<HistorySteps::Restore> HistorySteps::undo()
{
  if (m_undo.empty())
    return std::nullopt;
  Step step = std::move(m_undo.back());
  m_undo.pop_back();
  Restore restore{step.asset ? nlohmann::json() : nlohmann::json::parse(*step.before),
                  step.selectionBefore,
                  step.asset};
  m_redo.push_back(std::move(step));
  return restore;
}

std::optional<HistorySteps::Restore> HistorySteps::redo()
{
  if (m_redo.empty())
    return std::nullopt;
  Step step = std::move(m_redo.back());
  m_redo.pop_back();
  Restore restore{step.asset ? nlohmann::json() : nlohmann::json::parse(*step.after),
                  step.selectionAfter,
                  step.asset};
  m_undo.push_back(std::move(step));
  return restore;
}

void HistorySteps::clear()
{
  m_undo.clear();
  m_redo.clear();
  m_bytes = 0;
}

void HistorySteps::rewrite(const std::function<void(nlohmann::json&)>& edit)
{
  // Each scene once, still shared by the steps that shared it.
  std::unordered_map<const std::string*, Text> rewritten;
  const auto rewrite = [&](Text& text)
  {
    Text& done = rewritten[text.get()];
    if (!done)
    {
      nlohmann::json snapshot = nlohmann::json::parse(*text);
      edit(snapshot);
      done = HistorySteps::text(snapshot);
    }
    text = done;
  };
  m_bytes = 0;
  for (std::deque<Step>* steps : {&m_undo, &m_redo})
    for (Step& step : *steps)
    {
      if (step.asset)
      {
        m_bytes += step.bytes();
        continue;
      }
      rewrite(step.before);
      rewrite(step.after);
      m_bytes += step.bytes();
    }
  trim();
}

void HistorySteps::trim()
{
  // What could be redone goes first (it's the furthest from where the scene
  // is), then the oldest undo steps; the latest step always stays.
  while (!m_redo.empty() && m_bytes > m_maxBytes)
  {
    m_bytes -= m_redo.front().bytes();
    m_redo.pop_front();
  }
  while (m_undo.size() > 1 && (m_undo.size() > m_maxSteps || m_bytes > m_maxBytes))
  {
    m_bytes -= m_undo.front().bytes();
    m_undo.pop_front();
  }
}
