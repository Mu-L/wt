IF(ENABLE_LIBWTTEST)
  SET(TEST_SOURCES
    test.C
    any/AnyTest.C
    auth/BCryptTest.C
    auth/SHA1Test.C
    auth/TotpTest.C
    configuration/EntryPointTest.C
    core/BindTest.C
    core/ObservingPtrTest.C
    chart/WChartTest.C
    json/JsonParserTest.C
    json/JsonSerializerTest.C
    json/JsonValueTest.C
    http/CookieTest.C
    http/HttpClientTest.C
    formdelegate/WFormDelegate.C
    logger/ConcurentLoggingTest.C
    mail/MailClientTest.C
    mail/MailMessageTest.C
    matrix/MatrixTest.C
    models/WBatchEditProxyModelTest.C
    models/WFormModelTest.C
    models/WModelIndexTest.C
    models/WStandardItemModelTest.C
    private/EscapeTest.C
    private/EventDecodeTest.C
    private/HttpTest.C
    private/CExpressionParserTest.C
    private/ColorTest.C
    private/I18n.C
    private/SessionFromCookieTest.C
    private/UrlManipTest.C
    render/BlockCssPropertyTest.C
    render/CssParserTest.C
    render/CssSelectorTest.C
    render/SpecificityTest.C
    render/WTextRendererTest.C
    strings/WStringTest.C
    types/WDateTest.C
    types/WDateTimeTest.C
    utf8/Utf8Test.C
    utf8/XmlTest.C
    utils/Base32Test.C
    utils/Base64Test.C
    utils/DateUtils.C
    utils/EraseWord.C
    utils/HtmlEncode.C
    utils/InfraUtils.C
    utils/ParseNumber.C
    utils/RoundJsString.C
    utils/XmlUtils.C
    validators/WEmailValidator.C
    validators/WStackedValidator.C
    widgets/DisabledTest.C
    widgets/WCompositeWidgetTest.C
    widgets/WContainerWidgetTest.C
    widgets/WDateEditTest.C
    widgets/WSpinBoxTest.C
    widgets/WStackedWidgetTest.C
    widgets/WTemplateTest.C
    widgets/WTextTest.C
    widgets/WTimeEditTest.C
    widgets/WTreeViewTest.C
    length/WLengthTest.C
    color/WColorTest.C
    paintdevice/WSvgTest.C
    payment/MoneyTest.C
    locale/LocaleNumberTest.C
    trampoline/RefEncoder.C
    testenvironment/TestEnvironmentTest.C
    signals/SignalTest.C
    wdatetime/WDateTimeTest.C
    web/DomElementTest.C
  )

  set(thirdpartysources
    test.C
    thirdparty/qrcodegen/QrCode.C
  )

  add_executable(thirdpartytest.wt ${thirdpartysources})
  set_target_properties(thirdpartytest.wt PROPERTIES FOLDER "test/thirdparty")
  target_link_libraries(thirdpartytest.wt PRIVATE wt wttest ${BOOST_TEST_LIBRARIES})

  if(TARGET Boost::headers)
    target_link_libraries(thirdpartytest.wt PRIVATE Boost::headers)
  endif()

  if(WT_WITH_SSL)
    target_link_libraries(thirdpartytest.wt PRIVATE ${OPENSSL_LIBRARIES})
  endif()

  IF (WT_HAS_WRASTERIMAGE)
     SET(TEST_SOURCES ${TEST_SOURCES}
       paintdevice/WRasterTest.C
     )
  ENDIF(WT_HAS_WRASTERIMAGE)

  if(WT_WITH_SSL)
    set(TEST_SOURCES ${TEST_SOURCES} utils/SslUtils.C)
  endif()

  ADD_EXECUTABLE(test.wt
    ${TEST_SOURCES}
  )

  IF(MSVC)
    SET_TARGET_PROPERTIES(test.wt PROPERTIES FOLDER "test")
  ENDIF(MSVC)

  TARGET_LINK_LIBRARIES(test.wt PRIVATE wt wttest ${BOOST_TEST_LIBRARIES} ${BOOST_FS_LIB} ${WT_THREAD_LIB})

  IF(TARGET Boost::headers)
    TARGET_LINK_LIBRARIES(test.wt PRIVATE Boost::headers)
  ENDIF()

  if(WT_WITH_SSL)
    target_link_libraries(test.wt PRIVATE ${OPENSSL_LIBRARIES})
  endif()

  IF(ENABLE_LIBWTDBO)
    # Test all dbo backends
    SET(DBO_TEST_SOURCES
      test.C
      dbo/DboTest.C
      dbo/DboTest2.C
      dbo/DboTest3.C
      dbo/DboTest4.C
      dbo/DboTest5.C
      dbo/DboTest6.C
      dbo/DboTest7.C
      dbo/DboTest8.C
      dbo/DboTest9.C
      dbo/Benchmark.C
      dbo/Benchmark2.C
      dbo/JsonTest.C
      dbo/JsonTest2.C
      dbo/AuthDboTest.C
      dbo/DboTestCompositeKey.C
      auth/PasswordServiceTest.C
      private/DboImplTest.C
    )

    IF(HAVE_POSTGRES)
      ADD_EXECUTABLE(       test.postgres ${DBO_TEST_SOURCES})
      TARGET_LINK_LIBRARIES(test.postgres wt wttest wtdbo wtdbopostgres ${BOOST_TEST_LIBRARIES})
      SET_TARGET_PROPERTIES(test.postgres PROPERTIES COMPILE_FLAGS "-DPOSTGRES")
      IF(MSVC)
        SET_TARGET_PROPERTIES(test.postgres PROPERTIES FOLDER "test")
      ENDIF(MSVC)
      IF(TARGET Boost::headers)
        TARGET_LINK_LIBRARIES(test.postgres Boost::headers)
      ENDIF()
      MESSAGE("** Testing Wt::Dbo using Postgres backend")
    ENDIF(HAVE_POSTGRES)

    IF(HAVE_MYSQL)
      ADD_EXECUTABLE(       test.mysql ${DBO_TEST_SOURCES})
      TARGET_LINK_LIBRARIES(test.mysql wt wttest wtdbo wtdbomysql ${BOOST_TEST_LIBRARIES})
      SET_TARGET_PROPERTIES(test.mysql PROPERTIES COMPILE_FLAGS "-DMYSQL")
      IF(MSVC)
        SET_TARGET_PROPERTIES(test.mysql PROPERTIES FOLDER "test")
      ENDIF(MSVC)
      IF(TARGET Boost::headers)
        TARGET_LINK_LIBRARIES(test.mysql Boost::headers)
      ENDIF()
      MESSAGE("** Testing Wt::Dbo using Mysql backend")
    ENDIF(HAVE_MYSQL)

    IF(HAVE_ORACLE)
      ADD_EXECUTABLE(       test.oracle ${DBO_TEST_SOURCES})
      TARGET_LINK_LIBRARIES(test.oracle wt wttest wtdbo wtdbooracle ${BOOST_TEST_LIBRARIES})
      SET_TARGET_PROPERTIES(test.oracle PROPERTIES COMPILE_FLAGS "-DORACLE")
      IF(MSVC)
        SET_TARGET_PROPERTIES(test.oracle PROPERTIES FOLDER "test")
      ENDIF(MSVC)
      IF(TARGET Boost::headers)
        TARGET_LINK_LIBRARIES(test.oracle Boost::headers)
      ENDIF()
      MESSAGE("** Testing Wt::Dbo using Oracle backend")
    ENDIF(HAVE_ORACLE)

    IF(HAVE_FIREBIRD)
      ADD_EXECUTABLE(       test.firebird ${DBO_TEST_SOURCES})
      TARGET_LINK_LIBRARIES(test.firebird wt wttest wtdbo wtdbofirebird ${BOOST_TEST_LIBRARIES})
      SET_TARGET_PROPERTIES(test.firebird PROPERTIES COMPILE_FLAGS "-DFIREBIRD")
      IF(MSVC)
        SET_TARGET_PROPERTIES(test.firebird PROPERTIES FOLDER "test")
      ENDIF(MSVC)
      IF(TARGET Boost::headers)
        TARGET_LINK_LIBRARIES(test.firebird Boost::headers)
      ENDIF()
      MESSAGE("** Testing Wt::Dbo using Firebird backend")
    ENDIF(HAVE_FIREBIRD)

    IF(HAVE_SQLITE)
      ADD_EXECUTABLE(       test.sqlite3 ${DBO_TEST_SOURCES} dbo/SqliteTest.C)
      TARGET_LINK_LIBRARIES(test.sqlite3 wt wttest wtdbo wtdbosqlite3 ${BOOST_TEST_LIBRARIES})
      SET_TARGET_PROPERTIES(test.sqlite3 PROPERTIES COMPILE_FLAGS "-DSQLITE3")
      IF(MSVC)
        SET_TARGET_PROPERTIES(test.sqlite3 PROPERTIES FOLDER "test")
      ENDIF(MSVC)
      IF(TARGET Boost::headers)
        TARGET_LINK_LIBRARIES(test.sqlite3 Boost::headers)
      ENDIF()
      MESSAGE("** Testing Wt::Dbo using Sqlite3 backend")
    ENDIF(HAVE_SQLITE)

    IF(HAVE_MSSQLSERVER)
      ADD_EXECUTABLE(       test.mssqlserver ${DBO_TEST_SOURCES})
      TARGET_LINK_LIBRARIES(test.mssqlserver wt wttest wtdbo wtdbomssqlserver ${BOOST_TEST_LIBRARIES})
      SET_TARGET_PROPERTIES(test.mssqlserver PROPERTIES COMPILE_FLAGS "-DMSSQLSERVER")
      IF(MSVC)
        SET_TARGET_PROPERTIES(test.mssqlserver PROPERTIES FOLDER "test")
      ENDIF(MSVC)
      IF(TARGET Boost::headers)
          TARGET_LINK_LIBRARIES(test.mssqlserver Boost::headers)
      ENDIF()
      MESSAGE("** Testing Wt::Dbo using Microsoft SQL Server backend")
    ENDIF(HAVE_MSSQLSERVER)
  ENDIF(ENABLE_LIBWTDBO)

  INCLUDE_DIRECTORIES(${WT_SOURCE_DIR}/src)

  IF (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/interactive)
    SUBDIRS(interactive)
  ENDIF (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/interactive)

  IF(CONNECTOR_HTTP)
    SET(HTTP_TEST_SOURCES
      test.C
      http/HttpClientServerTest.C
    )

    ADD_EXECUTABLE(test.http ${HTTP_TEST_SOURCES})
    TARGET_LINK_LIBRARIES(test.http PRIVATE wt wthttp ${WT_THREAD_LIB} ${BOOST_TEST_LIBRARIES})
    IF(MSVC)
      SET_TARGET_PROPERTIES(test.http PROPERTIES FOLDER "test")
    ENDIF(MSVC)
    IF(TARGET Boost::headers)
      TARGET_LINK_LIBRARIES(test.http PRIVATE Boost::headers)
    ENDIF()
    if(WT_WITH_SSL)
      target_link_libraries(test.http PRIVATE ${OPENSSL_LIBRARIES})
    endif()
  ENDIF(CONNECTOR_HTTP)

ELSE (ENABLE_LIBWTTEST)
  MESSAGE("** libwttest disabled, not building tests")
ENDIF (ENABLE_LIBWTTEST)
