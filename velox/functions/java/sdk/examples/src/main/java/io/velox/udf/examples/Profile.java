package io.velox.udf.examples;

import io.velox.udf.Nullable;
import io.velox.udf.VeloxField;
import io.velox.udf.VeloxMap;
import io.velox.udf.VeloxRowType;
import java.util.List;

@VeloxRowType
public final class Profile {
  @VeloxField(index = 0, name = "name")
  public String name;

  @VeloxField(index = 1, name = "scores")
  public List<@Nullable Long> scores;

  @VeloxField(index = 2, name = "labels")
  public VeloxMap<String, @Nullable Long> labels;

  public Profile() {}
}
