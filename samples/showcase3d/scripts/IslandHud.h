#pragma once
#include <sfs/sfs.h>

#include <algorithm>
#include <string>

class IslandDirector;

// The heads-up display: sparks found, the time of day, what the station
// you're near shows, messages as things happen, and the controls at first.
class IslandHud : public sfs::ui::Layer
{
public:
  void setDirector(IslandDirector* director) { m_director = director; }

  sfs::ui::View body() override;

private:
  IslandDirector* m_director = nullptr; // the one that made it
};

#include "IslandDirector.h"

inline sfs::ui::View IslandHud::body()
{
  using namespace sfs::ui;
  if (!m_director)
    return Empty();

  const Color gold = Color::rgb(255, 214, 110);
  const Color sky = Color::rgb(170, 225, 255);
  const Color soft = Color::rgb(225, 228, 240);
  const Color panel = Color::rgb(12, 16, 30, 0.62f);

  // Sparks found, and the time of day.
  const float hours = m_director->timeOfDay() * 24.0f;
  const int hour = static_cast<int>(hours);
  const int minute = static_cast<int>((hours - hour) * 60.0f);
  const std::string clock = (hour < 10 ? "0" : "") + std::to_string(hour) +
                            ":" + (minute < 10 ? "0" : "") +
                            std::to_string(minute);
  std::vector<View> layers = {
      VStack(
          {Text("SPARKS " + std::to_string(m_director->collected()) + " / " +
                std::to_string(m_director->total()))
               .font(Font::title().sized(40))
               .color(gold),
           ProgressBar(m_director->total() > 0
                           ? static_cast<float>(m_director->collected()) /
                                 static_cast<float>(m_director->total())
                           : 0.0f)
               .frame(220, 6),
           Text((m_director->darkness() > 0.5f ? "Night  " : "Day  ") + clock)
               .font(Font::caption())
               .color(sky)},
          Alignment::Leading)
          .spacing(6)
          .padding(16)
          .background(panel)
          .cornerRadius(12)
          .padding(20)
          .align(Alignment::TopLeading)};

  // What the station nearby shows.
  if (const Station* station = m_director->nearStation())
    layers.push_back(
        VStack({Text(station->title).font(Font::headline()).color(gold),
                Text(station->hint).font(Font::body()).color(soft)})
            .spacing(4)
            .padding(16)
            .background(panel)
            .cornerRadius(12)
            .padding(28)
            .align(Alignment::Bottom));

  // Messages as things happen.
  if (m_director->messageLeft() > 0.0f)
    layers.push_back(
        Text(m_director->message())
            .font(Font::title().sized(m_director->won() ? 56 : 44))
            .color(m_director->won() ? gold : soft)
            .opacity(std::min(m_director->messageLeft() * 2.0f, 1.0f))
            .padding(18)
            .background(
                panel.opacity(std::min(m_director->messageLeft(), 1.0f)))
            .cornerRadius(14)
            .padding(80)
            .align(Alignment::Top));

  // The controls, for the first few seconds.
  if (m_director->clock() < 10.0f)
    layers.push_back(
        VStack({Text("W S  run    A D  turn").font(Font::caption()).color(soft),
                Text("Space  jump (twice in the air)")
                    .font(Font::caption())
                    .color(soft),
                Text("Shift  dash").font(Font::caption()).color(soft),
                Text("Drag the mouse  look around")
                    .font(Font::caption())
                    .color(soft),
                Text("T  hurry the day along     R  back to the start")
                    .font(Font::caption())
                    .color(soft)},
               Alignment::Leading)
            .spacing(4)
            .padding(14)
            .background(panel)
            .cornerRadius(12)
            .opacity(std::min((10.0f - m_director->clock()) * 0.5f, 1.0f))
            .padding(20)
            .align(Alignment::TopTrailing));

  return ZStack(std::move(layers));
}
