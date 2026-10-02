package io.velox.udf;

public final class VeloxTimestamp {
  private final long epochNanos;

  public VeloxTimestamp(long epochNanos) {
    this.epochNanos = epochNanos;
  }

  public long getEpochNanos() {
    return epochNanos;
  }

  @Override
  public boolean equals(Object other) {
    return other instanceof VeloxTimestamp
        && ((VeloxTimestamp) other).epochNanos == epochNanos;
  }

  @Override
  public int hashCode() {
    return (int) (epochNanos ^ (epochNanos >>> 32));
  }
}
