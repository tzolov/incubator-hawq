package com.pivotal.pxf.utilities;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.fail;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.when;

import org.apache.commons.logging.Log;
import org.apache.commons.logging.LogFactory;
import org.apache.hadoop.io.BooleanWritable;
import org.apache.hadoop.io.ByteWritable;
import org.apache.hadoop.io.DoubleWritable;
import org.apache.hadoop.io.FloatWritable;
import org.apache.hadoop.io.IntWritable;
import org.apache.hadoop.io.LongWritable;
import org.apache.hadoop.io.Text;
import org.apache.hadoop.io.Writable;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.Mockito;
import org.powermock.api.mockito.PowerMockito;
import org.powermock.core.classloader.annotations.PrepareForTest;
import org.powermock.modules.junit4.PowerMockRunner;

@RunWith(PowerMockRunner.class)
@PrepareForTest({RecordkeyAdapter.class, LogFactory.class})
public class RecordkeyAdapterTest {

	Log Log;
	RecordkeyAdapter recordkeyAdapter;

	/**
	 * Test convertKeyValue for Integer type
	 */
	@Test
	public void convertKeyValueInteger() {

		Integer key = new Integer(13);
		initRecordkeyAdapter();
		runConvertKeyValue(key, new IntWritable(key));
	}

	/**
	 * Test convertKeyValue for Boolean type
	 */
	@Test
	public void convertKeyValueBoolean() {

		Boolean key = new Boolean("true");
		initRecordkeyAdapter();
		runConvertKeyValue(key, new BooleanWritable(key));
	}

	/**
	 * Test convertKeyValue for Byte type
	 */
	@Test
	public void convertKeyValueByte() {

		Byte key = new Byte((byte) 1);
		initRecordkeyAdapter();
		runConvertKeyValue(key, new ByteWritable(key));
	}

	/**
	 * Test convertKeyValue for Double type
	 */
	@Test
	public void convertKeyValueDouble() {

		Double key = new Double(2.3);
		initRecordkeyAdapter();
		runConvertKeyValue(key, new DoubleWritable(key));
	}

	/**
	 * Test convertKeyValue for Float type
	 */
	@Test
	public void convertKeyValueFloat() {

		Float key = new Float(2.3);
		initRecordkeyAdapter();
		runConvertKeyValue(key, new FloatWritable(key));
	}

	/**
	 * Test convertKeyValue for Long type
	 */
	@Test
	public void convertKeyValueLong() {

		Long key = Long.parseLong("12345678901234567");
		initRecordkeyAdapter();
		runConvertKeyValue(key, new LongWritable(key));
	}

	/**
	 * Test convertKeyValue for String type
	 */
	@Test
	public void convertKeyValueString() {

		String key = "key";
		initRecordkeyAdapter();
		runConvertKeyValue(key, new Text(key));
	}

	/**
	 * Test convertKeyValue for several calls of the same type
	 */
	@Test
	public void convertKeyValueManyCalls() {

		
		Boolean key = new Boolean("true");
		mockLog();
		initRecordkeyAdapter();
		runConvertKeyValue(key, new BooleanWritable(key));
		verifyLog("converter initilized for type " + key.getClass() + 
			      " (key value: " + key + ")");
		
		for (int i = 0; i < 5; ++i) {
			key = new Boolean((i % 2) == 0);
			runConvertKeyValue(key, new BooleanWritable(key));
		}
		verifyLogOnlyOnce();
	}

	/**
	 * Test convertKeyValue for boolean type and then string type - negative
	 * test
	 */
	@Test
	public void convertKeyValueBadSecondValue() {

		Boolean key = new Boolean("true");
		initRecordkeyAdapter();
		runConvertKeyValue(key, new BooleanWritable(key));

		String badKey = "bad";
		try {
			recordkeyAdapter.convertKeyValue(badKey);
			fail("conversion of string to boolean should fail");
		} catch (ClassCastException e) {
			assertEquals(e.getMessage(),
						 "java.lang.String cannot be cast to java.lang.Boolean");
		}
	}

	private void initRecordkeyAdapter() {
		recordkeyAdapter = new RecordkeyAdapter();
	}
	
	private void runConvertKeyValue(Object key, Writable expected) {

		Writable writable = recordkeyAdapter.convertKeyValue(key);
		assertEquals(writable, expected);
	}
	
	private void mockLog() {
		PowerMockito.mockStatic(LogFactory.class);
		Log = mock(Log.class);
		when(LogFactory.getLog(RecordkeyAdapter.class)).thenReturn(Log);
	}
	
	private void verifyLog(String msg) {
		Mockito.verify(Log).debug(msg);
	}
	
	private void verifyLogOnlyOnce() {
		Mockito.verify(Log, Mockito.times(1)).debug(Mockito.any());
	}
}
