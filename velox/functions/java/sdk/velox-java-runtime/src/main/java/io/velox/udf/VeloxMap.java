package io.velox.udf;

public interface VeloxMap<K, V> extends Iterable<VeloxMap.Entry<K, V>> {
  int size();

  K keyAt(int index);

  V valueAt(int index);

  interface Entry<K, V> {
    K getKey();

    V getValue();
  }
}
