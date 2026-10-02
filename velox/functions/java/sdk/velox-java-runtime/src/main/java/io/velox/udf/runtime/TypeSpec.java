package io.velox.udf.runtime;

import java.lang.reflect.Field;

public final class TypeSpec {
  public enum Kind {
    BOOLEAN,
    TINYINT,
    SMALLINT,
    INTEGER,
    BIGINT,
    REAL,
    DOUBLE,
    VARCHAR,
    VARBINARY,
    DATE,
    TIMESTAMP,
    DECIMAL,
    ARRAY,
    MAP,
    ROW
  }

  public static final TypeSpec BOOLEAN = primitive(Kind.BOOLEAN);
  public static final TypeSpec TINYINT = primitive(Kind.TINYINT);
  public static final TypeSpec SMALLINT = primitive(Kind.SMALLINT);
  public static final TypeSpec INTEGER = primitive(Kind.INTEGER);
  public static final TypeSpec BIGINT = primitive(Kind.BIGINT);
  public static final TypeSpec REAL = primitive(Kind.REAL);
  public static final TypeSpec DOUBLE = primitive(Kind.DOUBLE);
  public static final TypeSpec VARCHAR = primitive(Kind.VARCHAR);
  public static final TypeSpec VARBINARY = primitive(Kind.VARBINARY);
  public static final TypeSpec DATE = primitive(Kind.DATE);
  public static final TypeSpec TIMESTAMP = primitive(Kind.TIMESTAMP);

  private final Kind kind;
  private final boolean nullable;
  private final int precision;
  private final int scale;
  private final TypeSpec elementType;
  private final TypeSpec keyType;
  private final TypeSpec valueType;
  private final Class<?> rowClass;
  private final String[] fieldNames;
  private final TypeSpec[] fieldTypes;
  private final Field[] javaFields;

  private TypeSpec(
      Kind kind,
      boolean nullable,
      int precision,
      int scale,
      TypeSpec elementType,
      TypeSpec keyType,
      TypeSpec valueType,
      Class<?> rowClass,
      String[] fieldNames,
      TypeSpec[] fieldTypes,
      Field[] javaFields) {
    this.kind = kind;
    this.nullable = nullable;
    this.precision = precision;
    this.scale = scale;
    this.elementType = elementType;
    this.keyType = keyType;
    this.valueType = valueType;
    this.rowClass = rowClass;
    this.fieldNames = fieldNames;
    this.fieldTypes = fieldTypes;
    this.javaFields = javaFields;
  }

  private static TypeSpec primitive(Kind kind) {
    return new TypeSpec(
        kind, false, 0, 0, null, null, null, null, null, null, null);
  }

  public static TypeSpec decimal(int precision, int scale) {
    if (precision < 1 || precision > 38 || scale < 0 || scale > precision) {
      throw new IllegalArgumentException("invalid Decimal128 precision/scale");
    }
    return new TypeSpec(
        Kind.DECIMAL, false, precision, scale,
        null, null, null, null, null, null, null);
  }

  public static TypeSpec array(TypeSpec elementType) {
    if (elementType == null) {
      throw new NullPointerException("array element type");
    }
    return new TypeSpec(
        Kind.ARRAY, false, 0, 0,
        elementType, null, null, null, null, null, null);
  }

  public static TypeSpec map(TypeSpec keyType, TypeSpec valueType) {
    if (keyType == null || valueType == null) {
      throw new NullPointerException("map key/value type");
    }
    if (keyType.isNullable()) {
      throw new IllegalArgumentException("Velox map key must be non-nullable");
    }
    return new TypeSpec(
        Kind.MAP, false, 0, 0,
        null, keyType, valueType, null, null, null, null);
  }

  public static TypeSpec row(
      Class<?> rowClass, String[] fieldNames, TypeSpec[] fieldTypes) {
    if (rowClass == null || fieldNames == null || fieldTypes == null
        || fieldNames.length != fieldTypes.length) {
      throw new IllegalArgumentException("invalid row type declaration");
    }
    Field[] javaFields = new Field[fieldNames.length];
    for (Field candidate : rowClass.getFields()) {
      io.velox.udf.VeloxField annotation =
          candidate.getAnnotation(io.velox.udf.VeloxField.class);
      if (annotation != null) {
        int index = annotation.index();
        if (index < 0 || index >= javaFields.length || javaFields[index] != null
            || !annotation.name().equals(fieldNames[index])) {
          throw new IllegalArgumentException(
              "invalid @VeloxField on " + rowClass.getName());
        }
        javaFields[index] = candidate;
      }
    }
    for (int i = 0; i < javaFields.length; ++i) {
      if (javaFields[i] == null || fieldTypes[i] == null) {
        throw new IllegalArgumentException(
            "missing @VeloxField index " + i + " on " + rowClass.getName());
      }
    }
    try {
      rowClass.getConstructor();
    } catch (NoSuchMethodException error) {
      throw new IllegalArgumentException(
          "row class needs a public no-argument constructor", error);
    }
    return new TypeSpec(
        Kind.ROW, false, 0, 0, null, null, null, rowClass,
        fieldNames.clone(), fieldTypes.clone(), javaFields);
  }

  public TypeSpec nullable() {
    return nullable ? this : new TypeSpec(
        kind, true, precision, scale, elementType, keyType, valueType,
        rowClass, fieldNames, fieldTypes, javaFields);
  }

  public Kind getKind() {
    return kind;
  }

  public boolean isNullable() {
    return nullable;
  }

  public int getPrecision() {
    return precision;
  }

  public int getScale() {
    return scale;
  }

  public TypeSpec getElementType() {
    return elementType;
  }

  public TypeSpec getKeyType() {
    return keyType;
  }

  public TypeSpec getValueType() {
    return valueType;
  }

  public Class<?> getRowClass() {
    return rowClass;
  }

  public int getFieldCount() {
    return fieldNames == null ? 0 : fieldNames.length;
  }

  public String getFieldName(int index) {
    return fieldNames[index];
  }

  public TypeSpec getFieldType(int index) {
    return fieldTypes[index];
  }

  public Object newRow() {
    try {
      return rowClass.newInstance();
    } catch (ReflectiveOperationException error) {
      throw new IllegalArgumentException("cannot construct " + rowClass.getName(), error);
    }
  }

  public Object getRowField(Object row, int index) {
    try {
      return javaFields[index].get(row);
    } catch (IllegalAccessException error) {
      throw new IllegalArgumentException("cannot read row field", error);
    }
  }

  public void setRowField(Object row, int index, Object value) {
    try {
      javaFields[index].set(row, value);
    } catch (IllegalAccessException error) {
      throw new IllegalArgumentException("cannot write row field", error);
    }
  }
}
