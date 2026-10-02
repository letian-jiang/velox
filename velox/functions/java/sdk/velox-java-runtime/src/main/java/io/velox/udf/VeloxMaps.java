package io.velox.udf;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Iterator;
import java.util.List;
import java.util.Objects;

public final class VeloxMaps {
  private VeloxMaps() {}

  public static <K, V> Builder<K, V> builder() {
    return new Builder<K, V>();
  }

  public static final class Builder<K, V> {
    private final List<VeloxMap.Entry<K, V>> entries =
        new ArrayList<VeloxMap.Entry<K, V>>();

    public Builder<K, V> put(K key, V value) {
      if (key == null) {
        throw new NullPointerException("Velox map key must not be null");
      }
      for (VeloxMap.Entry<K, V> entry : entries) {
        if (Objects.deepEquals(entry.getKey(), key)) {
          throw new IllegalArgumentException("duplicate Velox map key");
        }
      }
      entries.add(new ImmutableEntry<K, V>(key, value));
      return this;
    }

    public VeloxMap<K, V> build() {
      return new ImmutableVeloxMap<K, V>(entries);
    }
  }

  private static final class ImmutableEntry<K, V>
      implements VeloxMap.Entry<K, V> {
    private final K key;
    private final V value;

    private ImmutableEntry(K key, V value) {
      this.key = key;
      this.value = value;
    }

    @Override
    public K getKey() {
      return key;
    }

    @Override
    public V getValue() {
      return value;
    }
  }

  private static final class ImmutableVeloxMap<K, V>
      implements VeloxMap<K, V> {
    private final List<VeloxMap.Entry<K, V>> entries;

    private ImmutableVeloxMap(List<VeloxMap.Entry<K, V>> entries) {
      this.entries = Collections.unmodifiableList(
          new ArrayList<VeloxMap.Entry<K, V>>(entries));
    }

    @Override
    public int size() {
      return entries.size();
    }

    @Override
    public K keyAt(int index) {
      return entries.get(index).getKey();
    }

    @Override
    public V valueAt(int index) {
      return entries.get(index).getValue();
    }

    @Override
    public Iterator<VeloxMap.Entry<K, V>> iterator() {
      return entries.iterator();
    }
  }
}
