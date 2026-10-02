package io.velox.udf;

public final class VeloxDate32 {
  private final int days;

  public VeloxDate32(int days) {
    this.days = days;
  }

  public int getDays() {
    return days;
  }

  @Override
  public boolean equals(Object other) {
    return other instanceof VeloxDate32 && ((VeloxDate32) other).days == days;
  }

  @Override
  public int hashCode() {
    return days;
  }
}
