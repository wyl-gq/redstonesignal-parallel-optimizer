#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

template<typename T>
class nocheck_vector {
  static_assert(std::is_trivial_v<T>, "nocheck_vector requires trivial T");
  static_assert(std::is_trivially_copyable_v<T>, "nocheck_vector requires trivially copyable T");
  static_assert(std::is_trivially_constructible_v<T>, "nocheck_vector requires trivially constructible T");
  static_assert(std::is_trivially_destructible_v<T>, "nocheck_vector requires trivially destructible T");

private:
    T* _data;      // 指向分配内存的起始
    T* _current;   // 指向已构造元素的末尾(即 size 的末尾)
    T* _end;       // 指向分配内存的末尾(即 capacity 的末尾)

public:
    using value_type = T;
    using iterator = T*;
    using const_iterator = const T*;

    nocheck_vector();
    explicit nocheck_vector(size_t size);
    nocheck_vector(nocheck_vector&& other) noexcept ;
    nocheck_vector(const nocheck_vector &other);
    explicit nocheck_vector(const std::vector<T> &vec);
    explicit nocheck_vector(const std::span<T> &span);
    std::vector<T> to_vector() const;

    ~nocheck_vector();

    nocheck_vector& operator=(const nocheck_vector& other);
    nocheck_vector& operator=(nocheck_vector&& other) noexcept ;

    bool empty() const;
    void clear();
    void clear_reserve(size_t cap); // 清空数据 并设置空间大小为0

    size_t size() const;
    size_t capacity() const;

    void resize(size_t size);   
    void reserve(size_t cap);
    void reallocate(size_t cap); //清空数据 并设置空间大小为cap

    T pop();
    void pop_back() { pop(); };
    
    template<bool check = false>
    void push_back(const T& var);

    template<bool check = false>
    void push_back_empty();
    
    template<bool check = false>
    void push_back(const T& var, bool next);

    template<bool check = false>
    void insert(const T* first, const T* last);
    template<bool check = false>
    void insert(const T* first, size_t n);

    std::span<T> span() const { return std::span<T>(_data, size()); }
    std::span<T> subspan(size_t offset) const { return std::span<T>(_data + offset, size() - offset); }
    std::span<T> subspan(size_t offset, size_t count) const { return std::span<T>(_data + offset, count); }

    // 元素访问
    T& operator[](size_t idx) { return _data[idx]; }
    const T& operator[](size_t idx) const { return _data[idx]; }
    T* data() { return _data; }
    const T* data() const { return _data; }
    T* current() { return _current; }
    const T* current() const { return _current; }
    const T& front() const { return *_data; };
    const T& back() const { return *(_current - 1); };
    T& front() { return *_data; };
    T& back() { return *(_current - 1); };

    // 迭代器
    iterator begin() { return _data; }
    const_iterator begin() const { return _data; }
    iterator end() { return _current; }
    const_iterator end() const { return _current; }

    // 额外功能
    void sort_unique();
    void sort();

    // 扩展空间提供额外至少n的空间
    void grow(size_t n);
};

template<typename T>
nocheck_vector<T>::nocheck_vector() : _data(nullptr), _current(nullptr), _end(nullptr) {}

template <typename T> nocheck_vector<T>::nocheck_vector(size_t size) {
  if (size > 0) {
    _data = new T[size];
    _current = _data;
    _end = _data + size;
  } else {
    _data = _current = _end = nullptr;
  }
}

template <typename T>
nocheck_vector<T>::nocheck_vector(nocheck_vector &&other) noexcept : _data(other._data), _current(other._current), _end(other._end) {
  other._data = other._current = other._end = nullptr;
}

template <typename T>
nocheck_vector<T>::nocheck_vector(const nocheck_vector &other) {
  const size_t n = other.size();
  if (n > 0) {
    _data = new T[n];
    std::memcpy(_data, other._data, n * sizeof(T));
    _current = _data + n;
    _end = _current;
  } else {
    _data = _current = _end = nullptr;
  }
}

template <typename T> nocheck_vector<T>::~nocheck_vector() {  delete[] _data; }

template <typename T>
nocheck_vector<T> &nocheck_vector<T>::operator=(const nocheck_vector &other) {
  const size_t n = other.size();
  if (n == 0) {
    _current = _data;
    return *this;
  }

  if (capacity() >= n) {
    std::memcpy(_data, other._data, n * sizeof(T));
    _current = _data + n;
  } else {
    delete[] _data;
    _data = new T[n];
    std::memcpy(_data, other._data, n * sizeof(T));
    _current = _data + n;
    _end = _current;
  }

  return *this;
}

template <typename T>
nocheck_vector<T> &nocheck_vector<T>::operator=(nocheck_vector &&other)  noexcept {
  std::swap(_data, other._data);
  std::swap(_current, other._current);
  std::swap(_end, other._end);
  other.clear();

  return *this;
}

template <typename T>
nocheck_vector<T>::nocheck_vector(const std::vector<T> &vec) {
  const size_t n = vec.size();
  if (n > 0) {
    _data = new T[n];
    std::memcpy(_data, vec.data(), n * sizeof(T));
    _current = _data + n;
    _end = _current;
  } else {
    _data = _current = _end = nullptr;
  }
}

template <typename T>
nocheck_vector<T>::nocheck_vector(const std::span<T> &span) {
  const size_t n = span.size();
  if (n > 0) {
    _data = new T[n];
    std::memcpy(_data, span.data(), n * sizeof(T));
    _current = _data + n;
    _end = _current;
  } else {
    _data = _current = _end = nullptr;
  }
}


template <typename T> std::vector<T> nocheck_vector<T>::to_vector() const {
  return std::vector<T>(_data, _current);
}

template <typename T> bool nocheck_vector<T>::empty() const {
  return _data == _current;
}

template <typename T> void nocheck_vector<T>::clear() { _current = _data; }

template <typename T> void nocheck_vector<T>::clear_reserve(size_t cap) {
  clear();
  reserve(cap);
}

template <typename T> size_t nocheck_vector<T>::size() const {
  return static_cast<size_t>(_current - _data);
}

template <typename T> size_t nocheck_vector<T>::capacity() const {
  return static_cast<size_t>(_end - _data);
}

template <typename T> void nocheck_vector<T>::resize(size_t new_size) {
  if (new_size > capacity())
    reserve(new_size);

  _current = _data + new_size;
}

template <typename T> void nocheck_vector<T>::reserve(size_t new_cap) {
  if (new_cap <= capacity())
    return;

  T *new_data = new T[new_cap];
  const size_t old_size = size();
  if (old_size > 0) {
    std::memcpy(new_data, _data, old_size * sizeof(T));
  }

  delete[] _data;
  _data = new_data;
  _current = _data + old_size;
  _end = _data + new_cap;
}

template <typename T> void nocheck_vector<T>::reallocate(size_t cap) {
  delete[] _data;
  if (cap > 0) {
    _data = new T[cap];
    _current = _data;
    _end = _data + cap;
  } else {
    _data = _current = _end = nullptr;
  }
}

template <typename T>
T nocheck_vector<T>::pop() {
    --_current;
    return *_current;
}

template <typename T>
template <bool check>
void nocheck_vector<T>::push_back(const T &var) {
  if constexpr (check) {
    if (_current == _end)
      reserve(capacity() * 1.5 + 1);
  } 

  *_current = var;
  ++_current;
}

template <typename T>
template<bool check>
void nocheck_vector<T>::push_back_empty() {
  if constexpr (check) {
    if (_current == _end)
      reserve(capacity() * 1.5 + 1);
  } 
  ++_current;
}

template <typename T>
template <bool check>
void nocheck_vector<T>::push_back(const T &var, bool next) {
  if constexpr (check) {
    if (_current == _end)
      reserve(capacity() * 1.5 + 1);
  } 
  *_current = var;
  _current += next;
}

template <typename T>
template <bool check>
void nocheck_vector<T>::insert(const T *first, const T *last) {
  insert<check>(first, static_cast<size_t>(last - first));
}

template <typename T>
template <bool check>
void nocheck_vector<T>::insert(const T* first, size_t n) {
  if constexpr (check) {
    if (_current + n > _end)
      grow(n);
  } 
  std::memcpy(_current, first, n * sizeof(T));
  _current += n;
}



template <typename T> 
void nocheck_vector<T>::sort_unique() {
  static_assert(std::is_unsigned_v<T>,  "sort_unique only works for unsigned integers");
  if (size() <= 1) 
    return;

  sort();

  size_t not_unique_size = size();
  clear();

  T current = _data[0];
  for (size_t read = 1; read < not_unique_size; read++) {
    if (_data[read] != current) {
      push_back(current);
      current = _data[read];
    }
  }
  push_back(current);

}

template <typename T> 
void nocheck_vector<T>::sort() {
  static_assert(std::is_unsigned_v<T>,  "sort_unique only works for unsigned integers");
  size_t n = size();
  if (n <= 1)
    return;

  T *temp = new T[n]; // 临时缓冲区
  T *src = _data;     // 当前数据源
  T *dst = temp;      // 当前目标

  constexpr size_t byte_count = sizeof(T);
  for (size_t byte = 0; byte < byte_count; ++byte) {
    size_t count[256] = {0};

    // 统计当前字节的出现次数
    for (size_t i = 0; i < n; ++i) {
      uint8_t b = (src[i] >> (byte * 8)) & 0xFF;
      ++count[b];
    }
    // 计算前缀和（转换为起始位置）
    size_t sum = 0;
    for (size_t i = 0; i < 256; ++i) {
      size_t old = count[i];
      count[i] = sum;
      sum += old;
    }

    for (size_t i = 0; i < n; ++i) {
      uint8_t b = (src[i] >> (byte * 8)) & 0xFF;
      dst[count[b]] = src[i];
      ++count[b];
    }

    // 交换源和目标，准备下一趟
    std::swap(src, dst);
  }

  // 如果最终数据位于临时缓冲区，则复制回 _data
  if constexpr (byte_count % 2 == 1) {
    std::memcpy(_data, temp, n * sizeof(T));
  }
  // 否则数据已在 _data 中，无需操作

  delete[] temp;
}

template <typename T> void nocheck_vector<T>::grow(size_t n) {
  const size_t needed = size() + n;
  if (needed > capacity()) {
    const size_t new_cap = capacity() * 1.5 + 1;
    reserve(std::max(new_cap, needed));
  }
}