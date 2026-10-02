package io.velox.udf.runtime;

import io.velox.udf.VeloxDate32;
import io.velox.udf.VeloxMap;
import io.velox.udf.VeloxMaps;
import io.velox.udf.VeloxTimestamp;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.math.BigDecimal;
import java.math.RoundingMode;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import org.apache.arrow.memory.BufferAllocator;
import org.apache.arrow.memory.RootAllocator;
import org.apache.arrow.vector.BigIntVector;
import org.apache.arrow.vector.BitVector;
import org.apache.arrow.vector.DateDayVector;
import org.apache.arrow.vector.DecimalVector;
import org.apache.arrow.vector.FieldVector;
import org.apache.arrow.vector.Float4Vector;
import org.apache.arrow.vector.Float8Vector;
import org.apache.arrow.vector.IntVector;
import org.apache.arrow.vector.complex.ListVector;
import org.apache.arrow.vector.complex.MapVector;
import org.apache.arrow.vector.SmallIntVector;
import org.apache.arrow.vector.complex.StructVector;
import org.apache.arrow.vector.TimeStampNanoVector;
import org.apache.arrow.vector.TinyIntVector;
import org.apache.arrow.vector.UInt4Vector;
import org.apache.arrow.vector.VarBinaryVector;
import org.apache.arrow.vector.VarCharVector;
import org.apache.arrow.vector.VectorSchemaRoot;
import org.apache.arrow.vector.ipc.ArrowStreamReader;
import org.apache.arrow.vector.ipc.ArrowStreamWriter;
import org.apache.arrow.vector.types.Types;
import org.apache.arrow.vector.types.pojo.ArrowType;
import org.apache.arrow.vector.types.pojo.Field;
import org.apache.arrow.vector.types.pojo.FieldType;
import org.apache.arrow.vector.types.pojo.Schema;

public final class ArrowSupport {
  private static final RootAllocator ROOT = new RootAllocator(Long.MAX_VALUE);

  private ArrowSupport() {}

  public static final class InputBatch implements AutoCloseable {
    private final ArrowStreamReader reader;
    private final VectorSchemaRoot root;

    private InputBatch(ArrowStreamReader reader, VectorSchemaRoot root) {
      this.reader = reader;
      this.root = root;
    }

    public int getRowCount() {
      return root.getRowCount();
    }

    public FieldVector vector(int index) {
      if (index < 0 || index >= root.getFieldVectors().size()) {
        throw new IllegalArgumentException("missing Arrow input column " + index);
      }
      return root.getVector(index);
    }

    @Override
    public void close() throws Exception {
      reader.close();
    }
  }

  public static InputBatch decode(byte[] bytes) throws Exception {
    ArrowStreamReader reader =
        new ArrowStreamReader(new ByteArrayInputStream(bytes), ROOT);
    boolean loaded = false;
    try {
      if (!reader.loadNextBatch()) {
        throw new IllegalArgumentException("Arrow IPC contains no record batch");
      }
      loaded = true;
      return new InputBatch(reader, reader.getVectorSchemaRoot());
    } finally {
      if (!loaded) {
        reader.close();
      }
    }
  }

  public static Object read(
      InputBatch batch, int column, int row, TypeSpec type) {
    return readValue(batch.vector(column), row, type, "argument " + column);
  }

  private static Object readValue(
      FieldVector vector, int row, TypeSpec type, String description) {
    if (vector.isNull(row)) {
      if (!type.isNullable()) {
        throw new IllegalArgumentException(
            "non-null Java UDF " + description + " is null at row " + row);
      }
      return null;
    }
    switch (type.getKind()) {
      case BOOLEAN:
        require(vector, BitVector.class, description);
        return ((BitVector) vector).get(row) != 0;
      case TINYINT:
        require(vector, TinyIntVector.class, description);
        return ((TinyIntVector) vector).get(row);
      case SMALLINT:
        require(vector, SmallIntVector.class, description);
        return ((SmallIntVector) vector).get(row);
      case INTEGER:
        require(vector, IntVector.class, description);
        return ((IntVector) vector).get(row);
      case BIGINT:
        require(vector, BigIntVector.class, description);
        return ((BigIntVector) vector).get(row);
      case REAL:
        require(vector, Float4Vector.class, description);
        return ((Float4Vector) vector).get(row);
      case DOUBLE:
        require(vector, Float8Vector.class, description);
        return ((Float8Vector) vector).get(row);
      case VARCHAR:
        require(vector, VarCharVector.class, description);
        return new String(((VarCharVector) vector).get(row), StandardCharsets.UTF_8);
      case VARBINARY:
        require(vector, VarBinaryVector.class, description);
        return ((VarBinaryVector) vector).get(row);
      case DATE:
        require(vector, DateDayVector.class, description);
        return new VeloxDate32(((DateDayVector) vector).get(row));
      case TIMESTAMP:
        require(vector, TimeStampNanoVector.class, description);
        return new VeloxTimestamp(((TimeStampNanoVector) vector).get(row));
      case DECIMAL:
        require(vector, DecimalVector.class, description);
        DecimalVector decimal = (DecimalVector) vector;
        if (decimal.getPrecision() != type.getPrecision()
            || decimal.getScale() != type.getScale()) {
          throw new IllegalArgumentException(
              "decimal argument has precision/scale "
                  + decimal.getPrecision() + "/" + decimal.getScale()
                  + ", expected " + type.getPrecision() + "/" + type.getScale());
        }
        return decimal.getObject(row);
      case ARRAY:
        require(vector, ListVector.class, description);
        ListVector list = (ListVector) vector;
        List<Object> values = new ArrayList<Object>();
        int listStart = list.getElementStartIndex(row);
        int listEnd = list.getElementEndIndex(row);
        FieldVector elements = list.getDataVector();
        for (int index = listStart; index < listEnd; ++index) {
          values.add(readValue(
              elements, index, type.getElementType(), description + " element"));
        }
        return values;
      case MAP:
        require(vector, MapVector.class, description);
        MapVector map = (MapVector) vector;
        StructVector entries = (StructVector) map.getDataVector();
        FieldVector keys = (FieldVector) entries.getChildByOrdinal(0);
        FieldVector mapValues = (FieldVector) entries.getChildByOrdinal(1);
        VeloxMaps.Builder<Object, Object> builder = VeloxMaps.builder();
        int mapStart = map.getElementStartIndex(row);
        int mapEnd = map.getElementEndIndex(row);
        for (int index = mapStart; index < mapEnd; ++index) {
          builder.put(
              readValue(keys, index, type.getKeyType(), description + " key"),
              readValue(
                  mapValues, index, type.getValueType(), description + " value"));
        }
        return builder.build();
      case ROW:
        require(vector, StructVector.class, description);
        StructVector struct = (StructVector) vector;
        Object result = type.newRow();
        if (struct.getChildrenFromFields().size() != type.getFieldCount()) {
          throw new IllegalArgumentException(
              description + " has " + struct.getChildrenFromFields().size()
                  + " fields, expected "
                  + type.getFieldCount());
        }
        for (int index = 0; index < type.getFieldCount(); ++index) {
          FieldVector child = (FieldVector) struct.getChildByOrdinal(index);
          if (!child.getName().equals(type.getFieldName(index))) {
            throw new IllegalArgumentException(
                description + " row field " + index + " is named "
                    + child.getName() + ", expected " + type.getFieldName(index));
          }
          type.setRowField(
              result,
              index,
              readValue(
                  child,
                  row,
                  type.getFieldType(index),
                  description + "." + type.getFieldName(index)));
        }
        return result;
      default:
        throw new IllegalArgumentException("unsupported Java UDF input type");
    }
  }

  public static int readHandle(InputBatch batch, int column, int row) {
    FieldVector vector = batch.vector(column);
    require(vector, UInt4Vector.class, "column " + column);
    if (vector.isNull(row)) {
      throw new IllegalArgumentException("null Java UDAF state handle");
    }
    int handle = ((UInt4Vector) vector).get(row);
    if (handle == 0) {
      throw new IllegalArgumentException("invalid Java UDAF state handle 0");
    }
    return handle;
  }

  public static byte[] readBinary(InputBatch batch, int column, int row) {
    FieldVector vector = batch.vector(column);
    require(vector, VarBinaryVector.class, "column " + column);
    return vector.isNull(row) ? null : ((VarBinaryVector) vector).get(row);
  }

  private static void require(
      FieldVector vector,
      Class<? extends FieldVector> expected,
      String description) {
    if (!expected.isInstance(vector)) {
      throw new IllegalArgumentException(
          "Arrow " + description + " is " + vector.getClass().getSimpleName()
              + ", expected " + expected.getSimpleName());
    }
  }

  public static byte[] encodeValues(
      TypeSpec type, Object[] values, String[] errors) throws Exception {
    List<Field> fields = new ArrayList<Field>();
    fields.add(field("result", type, type.isNullable() || errors != null));
    if (errors != null) {
      fields.add(new Field(
          "__velox_java_error",
          FieldType.nullable(new ArrowType.Utf8()),
          null));
    }
    BufferAllocator allocator =
        ROOT.newChildAllocator("velox-java-output", 0, Long.MAX_VALUE);
    VectorSchemaRoot root = VectorSchemaRoot.create(new Schema(fields), allocator);
    try {
      root.allocateNew();
      FieldVector result = root.getVector(0);
      for (int row = 0; row < values.length; ++row) {
        if (values[row] == null) {
          result.setNull(row);
        } else {
          write(result, row, type, values[row]);
        }
      }
      result.setValueCount(values.length);
      if (errors != null) {
        VarCharVector errorVector = (VarCharVector) root.getVector(1);
        for (int row = 0; row < errors.length; ++row) {
          if (errors[row] == null) {
            errorVector.setNull(row);
          } else {
            errorVector.setSafe(
                row, errors[row].getBytes(StandardCharsets.UTF_8));
          }
        }
        errorVector.setValueCount(errors.length);
      }
      root.setRowCount(values.length);
      return encode(root);
    } finally {
      root.close();
      allocator.close();
    }
  }

  public static byte[] encodeBinary(byte[][] values) throws Exception {
    return encodeValues(TypeSpec.VARBINARY.nullable(), values, null);
  }

  private static byte[] encode(VectorSchemaRoot root) throws Exception {
    ByteArrayOutputStream output = new ByteArrayOutputStream();
    ArrowStreamWriter writer = new ArrowStreamWriter(root, null, output);
    try {
      writer.start();
      writer.writeBatch();
      writer.end();
    } finally {
      writer.close();
    }
    return output.toByteArray();
  }

  private static ArrowType arrowType(TypeSpec type) {
    switch (type.getKind()) {
      case BOOLEAN:
        return Types.MinorType.BIT.getType();
      case TINYINT:
        return Types.MinorType.TINYINT.getType();
      case SMALLINT:
        return Types.MinorType.SMALLINT.getType();
      case INTEGER:
        return Types.MinorType.INT.getType();
      case BIGINT:
        return Types.MinorType.BIGINT.getType();
      case REAL:
        return Types.MinorType.FLOAT4.getType();
      case DOUBLE:
        return Types.MinorType.FLOAT8.getType();
      case VARCHAR:
        return new ArrowType.Utf8();
      case VARBINARY:
        return new ArrowType.Binary();
      case DATE:
        return new ArrowType.Date(org.apache.arrow.vector.types.DateUnit.DAY);
      case TIMESTAMP:
        return new ArrowType.Timestamp(
            org.apache.arrow.vector.types.TimeUnit.NANOSECOND, null);
      case DECIMAL:
        return new ArrowType.Decimal(
            type.getPrecision(), type.getScale(), 128);
      case ARRAY:
        return new ArrowType.List();
      case MAP:
        return new ArrowType.Map(false);
      case ROW:
        return new ArrowType.Struct();
      default:
        throw new IllegalArgumentException("unsupported Java UDF output type");
    }
  }

  private static Field field(String name, TypeSpec type, boolean nullable) {
    List<Field> children = null;
    switch (type.getKind()) {
      case ARRAY:
        children = Arrays.asList(field(
            "item", type.getElementType(), type.getElementType().isNullable()));
        break;
      case MAP:
        Field key = field("key", type.getKeyType(), false);
        Field value = field(
            "value", type.getValueType(), type.getValueType().isNullable());
        Field entries = new Field(
            "entries",
            new FieldType(false, new ArrowType.Struct(), null),
            Arrays.asList(key, value));
        children = Arrays.asList(entries);
        break;
      case ROW:
        children = new ArrayList<Field>();
        for (int index = 0; index < type.getFieldCount(); ++index) {
          TypeSpec fieldType = type.getFieldType(index);
          children.add(field(
              type.getFieldName(index), fieldType, fieldType.isNullable()));
        }
        break;
      default:
        break;
    }
    return new Field(
        name, new FieldType(nullable, arrowType(type), null), children);
  }

  private static void write(
      FieldVector vector, int row, TypeSpec type, Object value) {
    switch (type.getKind()) {
      case BOOLEAN:
        ((BitVector) vector).setSafe(row, ((Boolean) value) ? 1 : 0);
        return;
      case TINYINT:
        ((TinyIntVector) vector).setSafe(row, ((Number) value).byteValue());
        return;
      case SMALLINT:
        ((SmallIntVector) vector).setSafe(row, ((Number) value).shortValue());
        return;
      case INTEGER:
        ((IntVector) vector).setSafe(row, ((Number) value).intValue());
        return;
      case BIGINT:
        ((BigIntVector) vector).setSafe(row, ((Number) value).longValue());
        return;
      case REAL:
        ((Float4Vector) vector).setSafe(row, ((Number) value).floatValue());
        return;
      case DOUBLE:
        ((Float8Vector) vector).setSafe(row, ((Number) value).doubleValue());
        return;
      case VARCHAR:
        ((VarCharVector) vector).setSafe(
            row, ((String) value).getBytes(StandardCharsets.UTF_8));
        return;
      case VARBINARY:
        ((VarBinaryVector) vector).setSafe(row, (byte[]) value);
        return;
      case DATE:
        ((DateDayVector) vector).setSafe(row, ((VeloxDate32) value).getDays());
        return;
      case TIMESTAMP:
        ((TimeStampNanoVector) vector).setSafe(
            row, ((VeloxTimestamp) value).getEpochNanos());
        return;
      case DECIMAL:
        BigDecimal decimal = ((BigDecimal) value).setScale(
            type.getScale(), RoundingMode.UNNECESSARY);
        if (decimal.precision() > type.getPrecision()) {
          throw new ArithmeticException(
              "decimal precision " + decimal.precision()
                  + " exceeds " + type.getPrecision());
        }
        ((DecimalVector) vector).setSafe(row, decimal);
        return;
      case ARRAY:
        ListVector list = (ListVector) vector;
        List<?> values = (List<?>) value;
        int listStart = list.startNewValue(row);
        for (int index = 0; index < values.size(); ++index) {
          writeNullable(
              list.getDataVector(),
              listStart + index,
              type.getElementType(),
              values.get(index));
        }
        list.endValue(row, values.size());
        return;
      case MAP:
        MapVector map = (MapVector) vector;
        VeloxMap<?, ?> valuesMap = (VeloxMap<?, ?>) value;
        int mapStart = map.startNewValue(row);
        StructVector entries = (StructVector) map.getDataVector();
        FieldVector keys = (FieldVector) entries.getChildByOrdinal(0);
        FieldVector mapValues = (FieldVector) entries.getChildByOrdinal(1);
        for (int index = 0; index < valuesMap.size(); ++index) {
          Object key = valuesMap.keyAt(index);
          if (key == null) {
            throw new NullPointerException("Velox map key must not be null");
          }
          int entry = mapStart + index;
          entries.setIndexDefined(entry);
          write(keys, entry, type.getKeyType(), key);
          writeNullable(
              mapValues, entry, type.getValueType(), valuesMap.valueAt(index));
        }
        map.endValue(row, valuesMap.size());
        return;
      case ROW:
        StructVector struct = (StructVector) vector;
        struct.setIndexDefined(row);
        for (int index = 0; index < type.getFieldCount(); ++index) {
          writeNullable(
              (FieldVector) struct.getChildByOrdinal(index),
              row,
              type.getFieldType(index),
              type.getRowField(value, index));
        }
        return;
      default:
        throw new IllegalArgumentException("unsupported Java UDF output type");
    }
  }

  private static void writeNullable(
      FieldVector vector, int row, TypeSpec type, Object value) {
    if (value == null) {
      if (!type.isNullable()) {
        throw new NullPointerException("non-null Java UDF value is null");
      }
      vector.setNull(row);
    } else {
      write(vector, row, type, value);
    }
  }
}
