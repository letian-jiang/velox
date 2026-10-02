package io.velox.udf.examples;

import io.velox.udf.Nullable;
import io.velox.udf.VeloxAggregate;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

@VeloxAggregate
public final class Average {
  public static final class State {
    double sum;
    long count;
  }

  public State create() {
    return new State();
  }

  public void destroy(State state) {}

  public void update(State state, @Nullable Double value) {
    if (value != null) {
      state.sum += value;
      state.count++;
    }
  }

  public byte[] serialize(State state) {
    return ByteBuffer.allocate(16)
        .order(ByteOrder.LITTLE_ENDIAN)
        .putDouble(state.sum)
        .putLong(state.count)
        .array();
  }

  public void merge(State state, byte[] intermediate) {
    if (intermediate.length != 16) {
      throw new IllegalArgumentException("invalid average intermediate state");
    }
    ByteBuffer buffer = ByteBuffer.wrap(intermediate).order(ByteOrder.LITTLE_ENDIAN);
    state.sum += buffer.getDouble();
    state.count += buffer.getLong();
  }

  @Nullable
  public Double finish(State state) {
    return state.count == 0 ? null : state.sum / state.count;
  }
}
