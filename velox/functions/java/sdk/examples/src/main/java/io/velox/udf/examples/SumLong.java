package io.velox.udf.examples;

import io.velox.udf.Nullable;
import io.velox.udf.VeloxAggregate;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

@VeloxAggregate
public final class SumLong {
  public static final class State {
    long sum;
    boolean seen;
  }

  public State create() {
    return new State();
  }

  public void destroy(State state) {}

  public void update(State state, @Nullable Long value) {
    if (value != null) {
      state.sum += value;
      state.seen = true;
    }
  }

  public byte[] serialize(State state) {
    return ByteBuffer.allocate(9)
        .order(ByteOrder.LITTLE_ENDIAN)
        .putLong(state.sum)
        .put((byte) (state.seen ? 1 : 0))
        .array();
  }

  public void merge(State state, byte[] intermediate) {
    if (intermediate.length != 9) {
      throw new IllegalArgumentException("invalid sum intermediate state");
    }
    ByteBuffer buffer = ByteBuffer.wrap(intermediate).order(ByteOrder.LITTLE_ENDIAN);
    state.sum += buffer.getLong();
    state.seen |= buffer.get() != 0;
  }

  @Nullable
  public Long finish(State state) {
    return state.seen ? state.sum : null;
  }
}
