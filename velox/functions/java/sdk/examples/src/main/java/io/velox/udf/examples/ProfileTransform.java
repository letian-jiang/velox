package io.velox.udf.examples;

import io.velox.udf.Nullable;
import io.velox.udf.VeloxMaps;
import io.velox.udf.VeloxScalar;
import java.util.ArrayList;
import java.util.Collections;

@VeloxScalar
public final class ProfileTransform {
  @Nullable
  public Profile evaluate(@Nullable Profile input) {
    if (input == null) {
      return null;
    }
    Profile result = new Profile();
    result.name = input.name.toUpperCase(java.util.Locale.ROOT);
    result.scores = new ArrayList<Long>(input.scores);
    Collections.reverse(result.scores);
    VeloxMaps.Builder<String, Long> labels = VeloxMaps.builder();
    for (int index = 0; index < input.labels.size(); ++index) {
      Long value = input.labels.valueAt(index);
      labels.put(input.labels.keyAt(index), value == null ? null : value + 1);
    }
    result.labels = labels.build();
    return result;
  }
}
