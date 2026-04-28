#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

class IPool
{
public:
  virtual ~IPool() = default;
};

template <typename T>
class Pool : public IPool
{
public:
  Pool() = default;
  virtual ~Pool() = default;

  bool isEmpty() const { return data.empty(); }
  size_t getSize() const { return data.size(); }
  void resize(size_t size) { data.resize(size); }
  void clear() { data.clear(); }

  template <typename... TArgs>
  void add(TArgs&&... args)
  {
    data.emplace_back(std::forward<TArgs>(args)...);
  }

  bool has(size_t index) const
  {
    return index < data.size() && data[index].has_value();
  }

  void remove(size_t index)
  {
    if (index < data.size())
    {
      data[index].reset();
    }
  }

  void remove(const T& obj)
  {
    for (size_t i = 0; i < data.size(); i++)
    {
      if (data[i] == obj)
      {

        data[i].reset();
        return;
      }
    }
  }

  template <typename... TArgs>
  void set(size_t index, TArgs&&... args)
  {
    if (index >= data.size())
    {
      data.resize(index + 1);
    }
    data[index].emplace(std::forward<TArgs>(args)...);
  }

  T& get(size_t index) { return data[index].value(); }

  T& operator[](size_t index) { return get(index); }

private:
  std::vector<std::optional<T>> data;
};
