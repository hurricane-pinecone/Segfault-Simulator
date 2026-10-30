#pragma once
#include <sfs/sfs.h>

#include <string>

class RunDirector;

// The run's heads-up display: score and distance, the combo running out, the
// best score, and the messages at the start and the end of a run.
class RunHud : public sfs::ui::Layer
{
public:
  void setDirector(RunDirector* director) { m_director = director; }

  sfs::ui::View body() override;

private:
  RunDirector* m_director = nullptr; // the one that made it
};

#include "RunDirector.h"

inline sfs::ui::View RunHud::body()
{
  using namespace sfs::ui;
  if (!m_director)
    return Empty();

  const Color cyan = Color::rgb(80, 240, 255);
  const Color magenta = Color::rgb(255, 80, 200);
  const Color yellow = Color::rgb(255, 226, 90);
  const Color panel = Color::rgb(10, 8, 26, 0.7f);

  std::vector<View> stats = {
      Text(std::to_string(m_director->score()))
          .font(Font::title().sized(44))
          .color(yellow),
      Text(std::to_string(m_director->distance()) + " m")
          .font(Font::headline())
          .color(cyan),
  };
  if (m_director->combo() > 1)
    stats.push_back(
        VStack({Text("COMBO x" + std::to_string(m_director->combo()))
                    .font(Font::headline())
                    .color(magenta),
                ProgressBar(m_director->comboLeft()).frame(160, 6)},
               Alignment::Leading)
            .spacing(4));
  if (m_director->best() > 0)
    stats.push_back(Text("BEST " + std::to_string(m_director->best()))
                        .font(Font::caption())
                        .color(Color::rgb(170, 180, 220)));

  std::vector<View> layers = {VStack(std::move(stats), Alignment::Leading)
                                  .spacing(6)
                                  .padding(16)
                                  .background(panel)
                                  .cornerRadius(10)
                                  .padding(20)
                                  .align(Alignment::TopLeading)};

  // A word at the start, and at the end.
  if (m_director->over())
    layers.push_back(
        VStack({Text("GLITCHED").font(Font::title().sized(72)).color(magenta),
                Text("press R, or wait").font(Font::headline()).color(cyan)})
            .spacing(8)
            .padding(28)
            .background(panel)
            .cornerRadius(14)
            .align(Alignment::Center));
  else if (m_director->clock() < 2.0f)
    layers.push_back(Text("RUN!")
                         .font(Font::title().sized(96))
                         .color(cyan)
                         .opacity(1.0f - m_director->clock() / 2.0f)
                         .align(Alignment::Center));
  return ZStack(std::move(layers));
}
