package org.apache.hawq.pxf.plugins.json;

/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

import java.io.IOException;
import java.util.ArrayList;
import java.util.EnumSet;
import java.util.List;
import java.util.Set;

import org.apache.commons.logging.Log;
import org.apache.commons.logging.LogFactory;
import org.apache.hawq.pxf.api.OneField;
import org.apache.hawq.pxf.api.OneRow;
import org.apache.hawq.pxf.api.ReadResolver;
import org.apache.hawq.pxf.api.io.DataType;
import org.apache.hawq.pxf.api.utilities.ColumnDescriptor;
import org.apache.hawq.pxf.api.utilities.InputData;
import org.apache.hawq.pxf.api.utilities.Plugin;

import com.jayway.jsonpath.Configuration;
import com.jayway.jsonpath.DocumentContext;
import com.jayway.jsonpath.JsonPath;
import com.jayway.jsonpath.Option;
import com.jayway.jsonpath.spi.json.JacksonJsonProvider;
import com.jayway.jsonpath.spi.json.JsonProvider;
import com.jayway.jsonpath.spi.mapper.JacksonMappingProvider;
import com.jayway.jsonpath.spi.mapper.MappingProvider;

/**
 * This JSON resolver for PXF will decode a given object from the {@link JsonAccessor} into a row for HAWQ. It will
 * decode this data into a JsonNode and walk the tree for each column. It supports normal value mapping via projections
 * and JSON array indexing.
 */
public class JsonResolverJsonPathExpression extends Plugin implements ReadResolver {

	private static final Log LOG = LogFactory.getLog(JsonResolverJsonPathExpression.class);

	private ArrayList<OneField> oneFieldList;
	private ColumnDescriptorCache[] columnDescriptorCache;

	/**
	 * Row with empty fields. Returned in case of broken or malformed json records.
	 */
	private final List<OneField> emptyRow;

	private Configuration jsonPathConfig;

	public JsonResolverJsonPathExpression(InputData inputData) throws Exception {
		super(inputData);
		oneFieldList = new ArrayList<OneField>();

		// Precompute the column metadata. The metadata is used for mapping column names to json nodes.
		columnDescriptorCache = new ColumnDescriptorCache[inputData.getColumns()];
		for (int i = 0; i < inputData.getColumns(); ++i) {
			ColumnDescriptor cd = inputData.getColumn(i);
			columnDescriptorCache[i] = new ColumnDescriptorCache(cd);
		}

		emptyRow = createEmptyRow();

		// this.initJsonConf();
		jsonPathConfig = Configuration.defaultConfiguration().setOptions(Option.DEFAULT_PATH_LEAF_TO_NULL)
				.setOptions(Option.SUPPRESS_EXCEPTIONS);

	}

	@Override
	public List<OneField> getFields(OneRow row) throws Exception {
		oneFieldList.clear();

		String jsonRecordAsText = row.getData().toString();

		DocumentContext jsonDocument =  null; 
		try {
			jsonDocument = JsonPath.using(jsonPathConfig).parse(jsonRecordAsText);
		} catch (Exception e) {
			LOG.debug(e);
		}
		
		if (jsonDocument == null) {
			LOG.warn("Return empty-fields row due to invalid JSON: " + jsonRecordAsText);
			return emptyRow;
		}

		// Iterate through the column definition and fetch our JSON data
		for (ColumnDescriptorCache columnMetadata : columnDescriptorCache) {

			Object value = JsonPath.using(jsonPathConfig).parse(jsonRecordAsText)
					.read("$." + columnMetadata.getColumnName());

			// If this node is null or missing, add a null value here
			if (value == null) {
				addNullField(columnMetadata.getColumnType());
			} else {
				addFieldFromJsonNode(columnMetadata.getColumnType(), value);
			}
		}

		return oneFieldList;
	}

	/**
	 * @return Returns a row comprised of typed, empty fields. Used as a result of broken/malformed json records.
	 */
	private List<OneField> createEmptyRow() {
		ArrayList<OneField> emptyFieldList = new ArrayList<OneField>();
		for (ColumnDescriptorCache column : columnDescriptorCache) {
			emptyFieldList.add(new OneField(column.getColumnType().getOID(), null));
		}
		return emptyFieldList;
	}



	/**
	 * Adds a field from a given JSON node value based on the {@link DataType} type.
	 * 
	 * @param type
	 *            The DataType type
	 * @param val
	 *            The JSON node to extract the value.
	 * @throws IOException
	 */
	private void addFieldFromJsonNode(DataType type, Object val) throws IOException {
		OneField oneField = new OneField();
		oneField.type = type.getOID();

		if (val == null) {
			oneField.val = null;
		} else {
			oneField.val = val;
//			switch (type) {
//			case BIGINT:
//				oneField.val = new Long((Integer) val);
//				break;
//			case BOOLEAN:
//				oneField.val = Boolean.valueOf((String)val);
//				break;
//			case CHAR:
//				oneField.val = ((String)val).charAt(0);
//				break;
//			case BYTEA:
//				oneField.val = ((String)val).getBytes();
//				break;
//			case FLOAT8:
//			case REAL:
//				oneField.val = (Double) val;
//				break;
//			case INTEGER:
//			case SMALLINT:
//				oneField.val = (Integer) val;
//				break;
//			case BPCHAR:
//			case TEXT:
//			case VARCHAR:
//				oneField.val = val;
//				break;
//			default:
//				throw new IOException("Unsupported type " + type);
//			}
		}

		oneFieldList.add(oneField);
	}

	/**
	 * Adds a null field of the given type.
	 * 
	 * @param type
	 *            The {@link DataType} type
	 */
	private void addNullField(DataType type) {
		oneFieldList.add(new OneField(type.getOID(), null));
	}
	
	private void initJsonConf() {
		Configuration.setDefaults(new Configuration.Defaults() {

		    private final JsonProvider jsonProvider = new JacksonJsonProvider();
		    private final MappingProvider mappingProvider = new JacksonMappingProvider();

		    @Override
		    public JsonProvider jsonProvider() {
		        return jsonProvider;
		    }

		    @Override
		    public MappingProvider mappingProvider() {
		        return mappingProvider;
		    }

		    @Override
		    public Set<Option> options() {
		        return EnumSet.noneOf(Option.class);
		    }
		});
	}
}